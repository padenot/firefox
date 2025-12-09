/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2  et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionBackend.h"

#include <speex/speex_resampler.h>

#include <utility>

#include "AudibilityMonitor.h"
#include "AudioConfig.h"
#include "AudioConverter.h"
#include "MainThreadUtils.h"
#include "SpeechRecognition.h"
#include "SpeechTrackListener.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/hwinference/HWInferenceManagerChild.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/SpeechRecognitionChild.h"
#include "nsCOMPtr.h"
#include "nsProxyRelease.h"
#include "nsString.h"

namespace mozilla::dom {

using namespace mozilla::ipc;

StaticRefPtr<nsIThread> SpeechRecognitionBackend::sIPCThread;
mozilla::EventTargetCapability<nsIThread>*
    SpeechRecognitionBackend::sIPCCapability = nullptr;
IPCThreadUserCounter SpeechRecognitionBackend::sIPCThreadUsers;
StaticRefPtr<mozilla::hwinference::HWInferenceManagerChild>
    SpeechRecognitionBackend::sHWInferenceChild;

void IPCThreadUserCounter::Increment() { mCount++; }

void IPCThreadUserCounter::Decrement() {
  MOZ_ASSERT(mCount > 0);
  mCount--;
  if (!mCount) {
    SpeechRecognitionBackend::StopIPCThreadIfPossible();
  }
}

bool IPCThreadUserCounter::IsZero() const { return !mCount; }

TransientSpeechRecognitionSession::TransientSpeechRecognitionSession()
    : mChild(SpeechRecognitionBackend::sHWInferenceChild
                 ->CreateSpeechRecognitionSession()) {
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "TransientSpeechRecognitionSession::Increment", []() {
        AssertIsOnMainThread();
        SpeechRecognitionBackend::sIPCThreadUsers.Increment();
      }));
}

TransientSpeechRecognitionSession::~TransientSpeechRecognitionSession() {
  SpeechRecognitionBackend::AssertOnIPCThread();
  if (mChild) {
    SpeechRecognitionChild::Send__delete__(mChild);
    mChild = nullptr;
  }
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "TransientSpeechRecognitionSession::Decrement", []() {
        AssertIsOnMainThread();
        SpeechRecognitionBackend::sIPCThreadUsers.Decrement();
      }));
}

static LazyLogModule gSpeechRecognitionBackendLog("SpeechRecognitionBackend");

#define LOG(fmt, ...)                                                      \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Debug, fmt, \
              ##__VA_ARGS__)
#define LOGV(fmt, ...)                                                       \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Verbose, fmt, \
              ##__VA_ARGS__)
#define LOGE(fmt, ...)                                                     \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Error, fmt, \
              ##__VA_ARGS__)

static constexpr double IPC_BLOCK_SIZE_S = 0.5;
static constexpr int32_t SPEECH_RECOGNITION_TARGET_RATE = 16000;
static constexpr auto SPEECH_RECOGNITION_ENGINE_ID = "whisper-cpp"_ns;

SpeechRecognitionBackend::SpeechRecognitionBackend(
    SpeechRecognition* aParent, uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases)
    : mParent(aParent),
      mLanguage(NS_ConvertUTF16toUTF8(aLanguage)),
      mPhrases(aPhrases.Clone()),
      mRingBuffer(MakeUnique<SPSCQueue<float>>(SPEECH_RECOGNITION_TARGET_RATE *
                                               IPC_BLOCK_SIZE_S * 4)),
      mResamplingCapability(NS_GetCurrentThread()),
      mMonoBuffer(512),
      mGraphRate(aGraphRate) {}

SpeechRecognitionBackend::~SpeechRecognitionBackend() {
  Abort();
}

nsresult SpeechRecognitionBackend::Start() {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Start");

  MOZ_ASSERT(!mSpeechRecognitionChild);

  mAudibilityMonitor = MakeUnique<AudibilityMonitor>(mGraphRate, 0.5f);
  mCurrentlyAudible = false;

  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self = RefPtr{this}](bool aSuccess) {
        AssertIsOnMainThread();
        if (!aSuccess) {
          LOGE("Failed to establish IPC connection in Start()");
          return;
        }
        OnIPCThread([self]() {
          AssertOnIPCThread();
          self->StartSpeechRecognitionSession(self->mLanguage);
        });
      },
      [self = RefPtr{this}](nsresult aError) {
        LOGE("IPC connection failed in Start(): {:x}",
             static_cast<uint32_t>(aError));
      });

  return NS_OK;
}

void SpeechRecognitionBackend::Stop() {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Stop");

  if (mResamplingThread) {
    RefPtr<SpeechRecognitionBackend> self = this;
    OnIPCThread([self = RefPtr{this}]() {
      AssertOnIPCThread();
      self->StopSpeechRecognitionSession();
    });
    mResamplingThreadRunning.store(false, std::memory_order_release);
    mResamplingThread->Shutdown();
    mResamplingThread = nullptr;
  }

  mMonoBuffer.Clear();

  RefPtr<SpeechRecognition> parent(mParent);
  if (!parent) {
    return;
  }

  if (mCurrentlyAudible) {
    nsCOMPtr<nsIRunnable> soundendRunnable = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::DispatchSoundEnd",
        [parent]() { parent->DispatchTrustedEvent(u"soundend"_ns); });
    NS_DispatchToMainThread(soundendRunnable.forget());
    mCurrentlyAudible = false;
  }

  nsCOMPtr<nsIRunnable> audioendRunnable = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::DispatchAudioEnd",
      [parent]() { parent->DispatchTrustedEvent(u"audioend"_ns); });
  NS_DispatchToMainThread(audioendRunnable.forget());
}

void SpeechRecognitionBackend::Abort() {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Abort");
  Stop();
}

void SpeechRecognitionBackend::AttachToTrack(AudioStreamTrack* aTrack) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aTrack);
  MOZ_ASSERT(!mTrack, "Already attached to a track");
  MOZ_ASSERT(!mTrackListener);

  mTrack = aTrack;
  mTrackListener = SpeechTrackListener::Create(this);
  mTrack->AddListener(mTrackListener);

  LOG("SpeechRecognitionBackend::AttachToTrack");
}

void SpeechRecognitionBackend::DetachFromTrack() {
  AssertIsOnMainThread();

  if (!mTrack) {
    return;
  }

  LOG("SpeechRecognitionBackend::DetachFromTrack");

  if (mTrackListener) {
    mTrack->RemoveListener(mTrackListener);
    mTrackListener = nullptr;
  }

  mTrack = nullptr;
}

void SpeechRecognitionBackend::DataCallback(TrackTime aTime,
                                            const AudioChunk& aChunk) {
  MOZ_ASSERT(!NS_IsMainThread(), "DataCallback must be on graph thread");

  if (aChunk.IsNull() || aChunk.mDuration == 0) {
    LOG("Null chunk in SpeechRecognitionBackend::DataCallback");
    return;
  }

  size_t frameCount = static_cast<size_t>(aChunk.mDuration);

  if (mMonoBuffer.Capacity() < frameCount) {
    LOGE("Warning: chunk size {} exceeds pre-allocated buffer capacity {}",
         frameCount, mMonoBuffer.Capacity());
    mMonoBuffer.SetLength(frameCount);
    MOZ_DIAGNOSTIC_CRASH("Implement chunked downmixing");
  }

  mMonoBuffer.SetLengthAndRetainStorage(frameCount);

  AudioDataValue* monoData = mMonoBuffer.Elements();
  Span<AudioDataValue* const> outputChannels(&monoData, 1);

  aChunk.DownMixTo(outputChannels);

  int written = mRingBuffer->Enqueue(mMonoBuffer.Elements(),
                                     AssertedCast<int>(frameCount));

  if (written < static_cast<int>(frameCount)) {
    LOG("Ring buffer overflow: wrote {} of {} frames", written, frameCount);
  }
}

void SpeechRecognitionBackend::StartProcessingAudioOnBackgroundThread() {
  AssertOnResamplingThread();

  ProcessAudioChunk();
}

void SpeechRecognitionBackend::ProcessAudioChunk() {
  mResamplingCapability.AssertOnCurrentThread();
  if (!mResamplingThreadRunning.load(std::memory_order_acquire)) {
    LOG("Background thread stopping, not scheduling next audio chunk");
    return;
  }

  LOGV("ProcessAudioChunk");

  if (!mAudioConverter) {
    AudioConfig inputConfig(1, mGraphRate, AudioConfig::FORMAT_FLT);
    AudioConfig outputConfig(1, SPEECH_RECOGNITION_TARGET_RATE,
                             AudioConfig::FORMAT_FLT);
    mAudioConverter = MakeUnique<AudioConverter>(inputConfig, outputConfig,
                                                 SPEEX_RESAMPLER_QUALITY_MIN);
  }

  int available = mRingBuffer->AvailableRead();
  double secondsAvailable = AssertedCast<double>(available) / mGraphRate;
  bool flushed = false;
  if (secondsAvailable > IPC_BLOCK_SIZE_S) {
    flushed = true;
    nsTArray<float> audioBuffer;
    audioBuffer.SetLength(available);
    int read = mRingBuffer->Dequeue(audioBuffer.Elements(), available);

    if (!mAudioStartDispatched) {
      mAudioStartDispatched = true;
      DispatchToParentIfAlive("SpeechRecognitionBackend::DispatchAudioStart",
                       [](SpeechRecognition* aParent) {
                         aParent->DispatchTrustedEvent(u"audiostart"_ns);
                       });
    }

    if (mAudibilityMonitor) {
      const float* audioData = audioBuffer.Elements();
      mAudibilityMonitor->ProcessPlanar(Span<const float* const>(&audioData, 1),
                                        read);

      bool nowAudible = mAudibilityMonitor->RecentlyAudible();
      if (nowAudible != mCurrentlyAudible) {
        mCurrentlyAudible = nowAudible;

        nsString eventName = nowAudible ? u"soundstart"_ns : u"soundend"_ns;
        DispatchToParentIfAlive("SpeechRecognitionBackend::DispatchSoundEvent",
                         [eventName](SpeechRecognition* aParent) {
                           aParent->DispatchTrustedEvent(eventName);
                         });
      }
    }

    nsTArray<float> resampledBuffer;
    mAudioConverter->Process(resampledBuffer, audioBuffer.Elements(), read);

    size_t frames = resampledBuffer.Length();

    LOGV("Sending {}s of audio via IPC",
         static_cast<float>(frames) / SPEECH_RECOGNITION_TARGET_RATE);
    SendAudioDataViaIPC(std::move(resampledBuffer));
  } else {
    LOGV("Not enough data in ringbuffer ({}s), retrying in a bit",
         secondsAvailable);
  }

  nsCOMPtr<nsIRunnable> nextChunk = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::ProcessAudioChunk", [self = RefPtr{this}]() {
        self->AssertOnResamplingThread();
        self->ProcessAudioChunk();
      });

  uint32_t nextProcessingTime =
      flushed ? AssertedCast<uint32_t>(IPC_BLOCK_SIZE_S * 1000) : 100;
  mResamplingThread->DelayedDispatch(nextChunk.forget(), nextProcessingTime);
}

void SpeechRecognitionBackend::SendAudioDataViaIPC(
    nsTArray<float>&& aAudioData) {
  AssertOnResamplingThread();

  RefPtr<SpeechRecognitionBackend> self = this;
  OnIPCThread([self, audioData = std::move(aAudioData)]() mutable {
    if (self->mSpeechRecognitionChild) {
      size_t sampleCount = audioData.Length();
      self->mSpeechRecognitionChild->SendProcessAudioData(std::move(audioData));
      LOGV("Sent {} samples to HWInference", sampleCount);
    } else {
      LOGE("SpeechRecognitionChild not available, dropping {} samples",
           audioData.Length());
    }
  });
}

void SpeechRecognitionBackend::StartSpeechRecognitionSession(
    const nsCString& aLanguage) {
  AssertOnIPCThread();

  mSpeechRecognitionChild = sHWInferenceChild->CreateSpeechRecognitionSession();

  mSpeechRecognitionChild->SetResultCallback(
      [self = RefPtr{this}](const nsCString& aTranscript, bool aIsFinal) {
        AssertOnIPCThread();
        LOG("Received recognition result: {} (final={})", aTranscript.get(),
            aIsFinal);

        self->HandleRecognitionResult(aTranscript, aIsFinal);
      });

  mSpeechRecognitionChild->SetErrorCallback(
      [self = RefPtr{this}](const nsCString& aError) {
        AssertOnIPCThread();
        LOGE("Recognition error: {}", aError.get());

        self->HandleRecognitionError(aError);
      });

  mSpeechRecognitionChild->SetSpeechChangeCallback(
      [self = RefPtr{this}](bool aSpeechDetected) {
        LOG("Speech change: {}", aSpeechDetected ? "started" : "ended");

        self->DispatchToParentIfAlive(
            "SpeechRecognitionBackend::HandleSpeechChange",
            [speechDetected = aSpeechDetected](SpeechRecognition* aParent) {
              aParent->DispatchTrustedEvent(speechDetected ? u"speechstart"_ns
                                                           : u"speechend"_ns);
            });
      });

  mSpeechRecognitionChild
      ->SendInit(SPEECH_RECOGNITION_ENGINE_ID, aLanguage, mPhrases)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}](bool aSuccess) {
            AssertOnIPCThread();
            if (!aSuccess) {
              LOGE(
                  "Failed to initialize speech recognition session - likely "
                  "another session is active");
              self->HandleRecognitionError(nsCString("concurrent-session"));
            } else {
              LOG("Speech recognition session initialized successfully");
              self->mResamplingThreadRunning.store(true,
                                                   std::memory_order_release);
              nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
                  "SpeechRecognitionBackend::ProcessAudioOnBackgroundThread",
                  [self]() {
                    self->AssertOnResamplingThread();
                    self->StartProcessingAudioOnBackgroundThread();
                  });
              nsresult rv = NS_NewNamedThread(
                  "SpeechResampler", getter_AddRefs(self->mResamplingThread),
                  runnable.forget());
              if (NS_FAILED(rv)) {
                LOGE("Failed to create background thread: {:x}",
                     static_cast<uint32_t>(rv));
                self->mResamplingThreadRunning.store(false,
                                                     std::memory_order_release);
              } else {
                self->mResamplingCapability =
                    EventTargetCapability<nsIThread>(self->mResamplingThread);
              }
            }
          },
          [self = RefPtr{this}](ResponseRejectReason aReason) {
            LOGE("Init IPC call failed: {}", static_cast<int>(aReason));
            AssertOnIPCThread();
            self->HandleRecognitionError(nsCString("network"));
          });
}

void SpeechRecognitionBackend::StopSpeechRecognitionSession() {
  AssertOnIPCThread();
  LOG("Stopping HWInference speech recognition session");
  mSpeechRecognitionChild->SendStop();
  SpeechRecognitionChild::Send__delete__(mSpeechRecognitionChild);
  mSpeechRecognitionChild = nullptr;
}

void SpeechRecognitionBackend::HandleRecognitionResult(
    const nsCString& aTranscript, bool aIsFinal) {
  MOZ_ASSERT(!NS_IsMainThread(), "Called from background thread");
  LOG("HandleRecognitionResult: {} (final={})", aTranscript.get(), aIsFinal);

  DispatchToParentIfAlive(
      "SpeechRecognitionBackend::HandleRecognitionResult",
      [transcript = nsCString(aTranscript), aIsFinal](SpeechRecognition* aParent) {
        aParent->HandleRecognitionResultFromBackend(transcript, aIsFinal);
      });
}

void SpeechRecognitionBackend::HandleRecognitionError(const nsCString& aError) {
  MOZ_ASSERT(!NS_IsMainThread(), "Called from background thread");
  LOGE("HandleRecognitionError: {}", aError.get());

  DispatchToParentIfAlive("SpeechRecognitionBackend::HandleRecognitionError",
                   [error = nsCString(aError)](SpeechRecognition* aParent) {
                     aParent->HandleRecognitionErrorFromBackend(error);
                   });
}

void SpeechRecognitionBackend::AssertOnResamplingThread() {
  MOZ_ASSERT(mResamplingThread->IsOnCurrentThread(),
             "Must be called on resampling thread");
}

void SpeechRecognitionBackend::NotifyTrackEnded() {
  DispatchToParentIfAlive("SpeechRecognitionBackend::NotifyTrackEnded",
                          [](SpeechRecognition* aParent) { aParent->Stop(); });
}

/* static */
nsCOMPtr<nsIThread> SpeechRecognitionBackend::GetOrCreateIPCThread() {
  AssertIsOnMainThread();

  if (!sIPCThread) {
    nsCOMPtr<nsIThread> thread;
    nsresult rv = NS_NewNamedThread("SpeechIPC", getter_AddRefs(thread));
    if (NS_SUCCEEDED(rv)) {
      sIPCThread = thread;
      sIPCCapability = new EventTargetCapability<nsIThread>(sIPCThread);
      LOG("Created shared IPC thread for speech recognition");
    } else {
      LOG("Failed to create shared IPC thread");
      return nullptr;
    }
  }

  return nsCOMPtr<nsIThread>(sIPCThread);
}

void SpeechRecognitionBackend::StopIPCThreadIfPossible() {
  AssertIsOnMainThread();
  if (sIPCThreadUsers.IsZero()) {
    nsCOMPtr<nsIThread> ipcThread = sIPCThread.forget();
    delete sIPCCapability;
    sIPCCapability = nullptr;
    ipcThread->Shutdown();
    LOG("Stopped shared IPC thread");
  }
}

/* static */
void SpeechRecognitionBackend::AssertOnIPCThread() {
  sIPCCapability->AssertOnCurrentThread();
}

/* static */
template <typename Func>
void SpeechRecognitionBackend::OnIPCThread(Func&& aFunc) {
  MOZ_ASSERT(sIPCThread, "Programming error: IPC thread not initialized");
  sIPCThread->Dispatch(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::OnIPCThread", std::forward<Func>(aFunc)));
}

template <typename Func>
void SpeechRecognitionBackend::DispatchToParentIfAlive(const char* aName,
                                                       Func&& aFunc) {
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      aName,
      [self = RefPtr{this}, aFunc = std::forward<Func>(aFunc)]() mutable {
        RefPtr<SpeechRecognition> parent(self->mParent);
        if (!parent) {
          return;
        }
        aFunc(parent.get());
      }));
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Available(
    nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages) {
  AssertIsOnMainThread();

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  nsTArray<nsCString> languages;
  for (const nsString& lang : aLanguages) {
    languages.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }
  if (languages.IsEmpty()) {
    languages.AppendElement("en-US"_ns);
  }

  LOG("SpeechRecognitionBackend::Available - Starting availability check for "
      "{} languages",
      languages.Length());

  for (const auto& lang : languages) {
    LOG("SpeechRecognitionBackend::Available - Language requested: {}",
        lang.get());
  }

  nsMainThreadPtrHandle<Promise> promiseHandle(
      MakeAndAddRef<nsMainThreadPtrHolder<Promise>>(
          "SpeechRecognitionBackend::Available", promise));

  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promiseHandle, languages = std::move(languages)](bool aSuccess) mutable {
        AssertIsOnMainThread();
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Available - Failed to initialize IPC");
          promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
          return;
        }

        OnIPCThread([promiseHandle,
                     languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Available - Connection ready, "
              "checking model availability");

          auto session = MakeUnique<TransientSpeechRecognitionSession>();

          if (!session->get()) {
            LOG("SpeechRecognitionBackend::Available - Failed to create speech "
                "recognition session");
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::ResolveUnavailable",
                [promiseHandle]() {
                  promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
                }));
            return;
          }

          session->get()->SendIsModelAvailable(languages)->Then(
              GetCurrentSerialEventTarget(), __func__,
              [promiseHandle, session = std::move(session)](bool available) {
                LOG("SpeechRecognitionBackend::Available - Received response: "
                    "{}",
                    available ? "true" : "false");
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveAvailable",
                    [promiseHandle, available]() {
                      if (available) {
                        promiseHandle->MaybeResolve(
                            AvailabilityStatus::Available);
                      } else {
                        promiseHandle->MaybeResolve(
                            AvailabilityStatus::Downloadable);
                      }
                    }));
              },
              [promiseHandle,
               session = std::move(session)](ResponseRejectReason reason) {
                LOG("SpeechRecognitionBackend::Available failed: {}",
                    static_cast<int>(reason));
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveUnavailable",
                    [promiseHandle]() {
                      promiseHandle->MaybeResolve(
                          AvailabilityStatus::Unavailable);
                    }));
              });
        });
      },
      [promiseHandle](nsresult aError) {
        LOG("SpeechRecognitionBackend::Available - IPC initialization failed");
        promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
      });

  return promise.forget();
}

/* static */
RefPtr<GenericPromise> SpeechRecognitionBackend::EnsureIPC() {
  AssertIsOnMainThread();

  // This ensures IPC has been setup. It requires two things:
  //
  // - a thread, on which all IPC calls are made
  // - effectively setting up the connection.
  //
  // Since sHWInferenceChild is SpeechIPC thread only, and this isn't
  // performance sensitive by any mean, we do this asynchronously. If not
  // set-up, the connection is established (on the main thread), then
  // sHWInferenceChild is initialized, and the promise is finally resolved.

  nsCOMPtr<nsIThread> ipcThread = GetOrCreateIPCThread();
  if (!ipcThread) {
    LOG("EnsureIPC - Failed to get IPC thread");
    return mozilla::GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  return InvokeAsync(ipcThread, __func__, []() -> RefPtr<GenericPromise> {
    AssertOnIPCThread();
    if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
      LOG("EnsureIPC - Connection already established (checked on IPC thread)");
      return GenericPromise::CreateAndResolve(true, __func__);
    }

    return InvokeAsync(
        GetMainThreadSerialEventTarget(), __func__,
        []() -> RefPtr<GenericPromise> {
          AssertIsOnMainThread();

          ContentChild* contentChild = ContentChild::GetSingleton();
          if (!contentChild) {
            LOG("EnsureIPC - No ContentChild available");
            return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
          }

          LOG("EnsureIPC - Creating endpoint pair");

          mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerParent> parentEp;
          mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerChild> childEp;

          MOZ_ALWAYS_SUCCEEDS(
              hwinference::PHWInferenceManager::CreateEndpoints(&parentEp, &childEp));
          DebugOnly<bool> ok = contentChild->SendRequestHWInferenceConnection(
              std::move(parentEp));
          MOZ_ASSERT(ok);

          return InvokeAsync(
              sIPCThread, __func__,
              [childEp =
                   std::move(childEp)]() mutable -> RefPtr<GenericPromise> {
                AssertOnIPCThread();

                LOG("EnsureIPC - Opening connection on the SpeechIPC thread");
                [&]() MOZ_NO_THREAD_SAFETY_ANALYSIS {
                  mozilla::hwinference::HWInferenceManagerChild::OpenForProcess(std::move(childEp));
                }();
                sHWInferenceChild = mozilla::hwinference::HWInferenceManagerChild::GetSingleton();

                if (sHWInferenceChild->CanSend()) {
                  LOG("EnsureIPC - Connection established");
                  return GenericPromise::CreateAndResolve(true, __func__);
                }
                LOG("EnsureIPC - Failed to establish");
                return GenericPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                       __func__);
              });
        });
  });
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Install(
    nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages) {
  AssertIsOnMainThread();

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  if (aLanguages.IsEmpty()) {
    promise->MaybeResolve(false);
    return promise.forget();
  }

  nsTArray<nsCString> languages;
  for (const nsString& lang : aLanguages) {
    languages.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }

  LOG("SpeechRecognitionBackend::Install - Starting install for {} languages",
      languages.Length());

  nsMainThreadPtrHandle<Promise> promiseHandle(
      MakeAndAddRef<nsMainThreadPtrHolder<Promise>>(
          "SpeechRecognitionBackend::Install", promise));

  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promiseHandle, languages = std::move(languages)](bool aSuccess) mutable {
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Install - Failed to initialize IPC");
          promiseHandle->MaybeResolve(false);
          return;
        }

        OnIPCThread([promiseHandle,
                     languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Install - Connection ready, starting "
              "model installation");

          auto session = MakeUnique<TransientSpeechRecognitionSession>();

          session->get()
              ->SendInstallModels(std::move(languages))
              ->Then(
                  GetCurrentSerialEventTarget(), __func__,
                  [promiseHandle, session = std::move(session)](bool success) {
                    LOG("SpeechRecognitionBackend::Install - Install "
                        "completed: {}",
                        success ? "success" : "failed");
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstall",
                        [promiseHandle, success]() {
                          promiseHandle->MaybeResolve(success);
                        }));
                  },
                  [promiseHandle,
                   session = std::move(session)](ResponseRejectReason aReason) {
                    LOG("SpeechRecognitionBackend::Install - Install failed "
                        "with reason: {}",
                        static_cast<int>(aReason));
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstallFailed",
                        [promiseHandle]() {
                          promiseHandle->MaybeResolve(false);
                        }));
                  });
        });
      },
      [promiseHandle](nsresult aError) {
        LOG("SpeechRecognitionBackend::Install - IPC initialization failed");
        promiseHandle->MaybeResolve(false);
      });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
