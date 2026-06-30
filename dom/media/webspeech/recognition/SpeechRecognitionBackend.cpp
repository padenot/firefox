/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2  et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionBackend.h"

#include <speex/speex_resampler.h>

#include <algorithm>
#include <utility>

#include "AudibilityMonitor.h"
#include "AudioConfig.h"
#include "AudioConverter.h"
#include "MainThreadUtils.h"
#include "SpeechRecognition.h"
#include "SpeechTrackListener.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Assertions.h"
#include "mozilla/SpeechRecognitionChild.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/hwinference/HWInferenceManagerChild.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/ProtocolUtils.h"
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
                 ->CreateSpeechRecognitionSession()) {}

TransientSpeechRecognitionSession::~TransientSpeechRecognitionSession() {
  SpeechRecognitionBackend::AssertOnIPCThread();
  if (mChild) {
    SpeechRecognitionChild::Send__delete__(mChild);
    mChild = nullptr;
  }
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

// Releases the IPC-thread hold taken by a transient operation once its promise
// settles, matching the AcquireIPCThreadUser() call made when it started.
class IPCThreadUserReleaser final : public PromiseNativeHandler {
 public:
  NS_DECL_ISUPPORTS

  void ResolvedCallback(JSContext*, JS::Handle<JS::Value>,
                        ErrorResult&) override {
    SpeechRecognitionBackend::ReleaseIPCThreadUser();
  }
  void RejectedCallback(JSContext*, JS::Handle<JS::Value>,
                        ErrorResult&) override {
    SpeechRecognitionBackend::ReleaseIPCThreadUser();
  }

 private:
  ~IPCThreadUserReleaser() = default;
};

NS_IMPL_ISUPPORTS0(IPCThreadUserReleaser)

// Audio is streamed to the inference process in small blocks to keep end-to-end
// latency low. The block is well under the model's internal chunk (~160 ms), so
// the buffering delay is hidden behind the model's own latency rather than
// adding to it. (Was 0.5 s when the backend drove whisper.cpp's offline batch
// path.) The ring buffer is sized independently, with generous headroom against
// resampling-thread scheduling jitter.
static constexpr double IPC_BLOCK_SIZE_S = 0.04;
static constexpr double RING_BUFFER_SIZE_S = 0.5;
static constexpr uint32_t STREAMING_POLL_MS = 20;
static constexpr int32_t SPEECH_RECOGNITION_TARGET_RATE = 16000;
static constexpr auto SPEECH_RECOGNITION_ENGINE_ID = "whisper-cpp"_ns;

SpeechRecognitionBackend::SpeechRecognitionBackend(
    SpeechRecognition* aParent, uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases)
    : mParent(aParent),
      mLanguage(NS_ConvertUTF16toUTF8(aLanguage)),
      mPhrases(aPhrases.Clone()),
      mRingBuffer(MakeUnique<SPSCQueue<float>>(SPEECH_RECOGNITION_TARGET_RATE *
                                               RING_BUFFER_SIZE_S * 4)),
      mResamplingCapability(NS_GetCurrentThread()),
      mMonoBuffer(512),
      mGraphRate(aGraphRate) {}

SpeechRecognitionBackend::~SpeechRecognitionBackend() { Abort(); }

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

  // Idempotent: ~SpeechRecognitionBackend() also runs Abort()->Stop(). Once the
  // teardown below has run, it must not run again from the destructor, where
  // capturing RefPtr{this} would resurrect an object already at refcount zero
  // and double-free it.
  if (mStopped) {
    return;
  }
  mStopped = true;

  // Tear down the IPC session unconditionally, not only once the resampling
  // thread exists. Start() may be stopped or aborted (including via
  // ~SpeechRecognitionBackend) while session init is still in flight, before
  // the resampling thread is created. Skipping teardown in that window leaks
  // the inference process' single-session slot and makes the next session fail
  // with a spurious concurrent-session error.
  if (sIPCThread) {
    OnIPCThread([self = RefPtr{this}]() {
      AssertOnIPCThread();
      self->StopSpeechRecognitionSession();
    });
  }

  if (mResamplingThread) {
    mResamplingThreadRunning.store(false, std::memory_order_release);
    mResamplingThread->Shutdown();
    mResamplingThread = nullptr;
  }

  // mMonoBuffer is graph-thread scratch (used only in DataCallback); it must
  // not be touched from the main thread here, as DataCallback may still run
  // until the track listener is removed. It is freed when this backend is
  // destroyed.

  RefPtr<SpeechRecognition> parent(mParent);
  if (!parent) {
    return;
  }

  if (mCurrentlyAudible) {
    TimeStamp soundendTs = TimeStamp::Now();
    nsCOMPtr<nsIRunnable> soundendRunnable = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::DispatchSoundEnd", [parent, soundendTs]() {
          parent->DispatchTrustedEventWithTimestamp(u"soundend"_ns, soundendTs);
        });
    NS_DispatchToMainThread(soundendRunnable.forget());
    mCurrentlyAudible = false;
  }

  TimeStamp audioendTs = TimeStamp::Now();
  nsCOMPtr<nsIRunnable> audioendRunnable = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::DispatchAudioEnd", [parent, audioendTs]() {
        parent->DispatchTrustedEventWithTimestamp(u"audioend"_ns, audioendTs);
      });
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

  const size_t frameCount = static_cast<size_t>(aChunk.mDuration);

  // Downmix to mono into the fixed-size scratch buffer and enqueue. A single
  // graph chunk can be larger than the scratch buffer, so process it in slices
  // that fit, avoiding any allocation on the real-time graph thread.
  AudioDataValue* monoData = mMonoBuffer.Elements();
  Span<AudioDataValue* const> outputChannels(&monoData, 1);
  const size_t capacity = mMonoBuffer.Capacity();

  for (size_t offset = 0; offset < frameCount; offset += capacity) {
    const size_t sliceFrames = std::min(capacity, frameCount - offset);
    mMonoBuffer.SetLengthAndRetainStorage(sliceFrames);

    AudioChunk slice = aChunk;
    slice.SliceTo(offset, offset + sliceFrames);
    slice.DownMixTo(outputChannels);

    int written = mRingBuffer->Enqueue(mMonoBuffer.Elements(),
                                       AssertedCast<int>(sliceFrames));
    if (written < static_cast<int>(sliceFrames)) {
      LOG("Ring buffer overflow: wrote {} of {} frames", written, sliceFrames);
    }
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
  if (secondsAvailable > IPC_BLOCK_SIZE_S) {
    nsTArray<float> audioBuffer;
    audioBuffer.SetLength(available);
    int read = mRingBuffer->Dequeue(audioBuffer.Elements(), available);

    if (!mAudioStartDispatched) {
      mAudioStartDispatched = true;
      TimeStamp audioStartTs = TimeStamp::Now();
      DispatchToParentIfAlive("SpeechRecognitionBackend::DispatchAudioStart",
                              [audioStartTs](SpeechRecognition* aParent) {
                                aParent->DispatchTrustedEventWithTimestamp(
                                    u"audiostart"_ns, audioStartTs);
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
        TimeStamp soundTs = TimeStamp::Now();
        DispatchToParentIfAlive(
            "SpeechRecognitionBackend::DispatchSoundEvent",
            [eventName, soundTs](SpeechRecognition* aParent) {
              aParent->DispatchTrustedEventWithTimestamp(eventName, soundTs);
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

  mResamplingThread->DelayedDispatch(nextChunk.forget(), STREAMING_POLL_MS);
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

  if (mStopRequested) {
    // Stop()/Abort() ran before this init task reached the IPC thread; do not
    // open a session that nobody will tear down.
    LOG("Session init skipped, teardown already requested");
    return;
  }

  mSpeechRecognitionChild = sHWInferenceChild->CreateSpeechRecognitionSession();

  mSpeechRecognitionChild->SetResultCallback(
      [self = RefPtr{this}](const nsCString& aTranscript, bool aIsFinal,
                            float aConfidence, TimeStamp aEventTime) {
        AssertOnIPCThread();
        LOG("Received recognition result: {} (final={})", aTranscript.get(),
            aIsFinal);

        self->HandleRecognitionResult(aTranscript, aIsFinal, aConfidence,
                                      aEventTime);
      });

  mSpeechRecognitionChild->SetErrorCallback(
      [self = RefPtr{this}](const nsCString& aError) {
        AssertOnIPCThread();
        LOGE("Recognition error: {}", aError.get());

        self->HandleRecognitionError(aError);
      });

  mSpeechRecognitionChild->SetSpeechChangeCallback(
      [self = RefPtr{this}](bool aSpeechDetected, TimeStamp aEventTime) {
        LOG("Speech change: {}", aSpeechDetected ? "started" : "ended");

        self->DispatchToParentIfAlive(
            "SpeechRecognitionBackend::HandleSpeechChange",
            [speechDetected = aSpeechDetected,
             aEventTime](SpeechRecognition* aParent) {
              aParent->DispatchTrustedEventWithTimestamp(
                  speechDetected ? u"speechstart"_ns : u"speechend"_ns,
                  aEventTime);
            });
      });

  mSpeechRecognitionChild
      ->SendInit(SPEECH_RECOGNITION_ENGINE_ID, aLanguage, mPhrases)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}](const nsCString& aError) {
            AssertOnIPCThread();
            if (!aError.IsEmpty()) {
              LOGE("Failed to initialize speech recognition session: {}",
                   aError.get());
              self->HandleRecognitionError(aError);
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
  mStopRequested = true;
  if (!mSpeechRecognitionChild) {
    // Session init has not reached the IPC thread yet (or already failed);
    // mStopRequested above keeps it from opening a leaked session.
    return;
  }
  LOG("Stopping HWInference speech recognition session");
  mSpeechRecognitionChild->SendStop();
  SpeechRecognitionChild::Send__delete__(mSpeechRecognitionChild);
  mSpeechRecognitionChild = nullptr;
}

void SpeechRecognitionBackend::HandleRecognitionResult(
    const nsCString& aTranscript, bool aIsFinal, float aConfidence,
    TimeStamp aEventTime) {
  MOZ_ASSERT(!NS_IsMainThread(), "Called from background thread");
  LOG("HandleRecognitionResult: {} (final={})", aTranscript.get(), aIsFinal);

  DispatchToParentIfAlive(
      "SpeechRecognitionBackend::HandleRecognitionResult",
      [transcript = nsCString(aTranscript), aIsFinal, aConfidence,
       aEventTime](SpeechRecognition* aParent) {
        aParent->HandleRecognitionResultFromBackend(transcript, aIsFinal,
                                                    aConfidence, aEventTime);
      });
}

void SpeechRecognitionBackend::HandleRecognitionError(const nsCString& aError) {
  MOZ_ASSERT(!NS_IsMainThread(), "Called from background thread");
  LOGE("HandleRecognitionError: {}", aError.get());

  DispatchToParentIfAlive(
      "SpeechRecognitionBackend::HandleRecognitionError",
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
void SpeechRecognitionBackend::AcquireIPCThreadUser() {
  AssertIsOnMainThread();
  sIPCThreadUsers.Increment();
}

/* static */
void SpeechRecognitionBackend::ReleaseIPCThreadUser() {
  AssertIsOnMainThread();
  sIPCThreadUsers.Decrement();
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

  // Hold the shared IPC thread for the whole operation, releasing when the
  // promise settles.
  AcquireIPCThreadUser();
  promise->AppendNativeHandler(new IPCThreadUserReleaser());

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
already_AddRefed<Promise> SpeechRecognitionBackend::GetModelDownloadSize(
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

  AcquireIPCThreadUser();
  promise->AppendNativeHandler(new IPCThreadUserReleaser());

  nsTArray<nsCString> languages;
  for (const nsString& lang : aLanguages) {
    languages.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }

  nsMainThreadPtrHandle<Promise> promiseHandle(
      MakeAndAddRef<nsMainThreadPtrHolder<Promise>>(
          "SpeechRecognitionBackend::GetModelDownloadSize", promise));

  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promiseHandle, languages = std::move(languages)](bool aSuccess) mutable {
        AssertIsOnMainThread();
        if (!aSuccess) {
          promiseHandle->MaybeResolve(0);
          return;
        }
        OnIPCThread([promiseHandle,
                     languages = std::move(languages)]() mutable {
          auto session = MakeUnique<TransientSpeechRecognitionSession>();
          if (!session->get()) {
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::ResolveSizeZero",
                [promiseHandle]() { promiseHandle->MaybeResolve(0); }));
            return;
          }
          session->get()->SendGetModelDownloadSize(languages)->Then(
              GetCurrentSerialEventTarget(), __func__,
              [promiseHandle, session = std::move(session)](uint32_t aSizeMB) {
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveSize",
                    [promiseHandle, aSizeMB]() {
                      promiseHandle->MaybeResolve(aSizeMB);
                    }));
              },
              [promiseHandle,
               session = std::move(session)](ResponseRejectReason reason) {
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveSizeZero",
                    [promiseHandle]() { promiseHandle->MaybeResolve(0); }));
              });
        });
      },
      [promiseHandle](nsresult aError) { promiseHandle->MaybeResolve(0); });

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

          mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerParent>
              parentEp;
          mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerChild> childEp;

          MOZ_ALWAYS_SUCCEEDS(hwinference::PHWInferenceManager::CreateEndpoints(
              &parentEp, &childEp));
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
                  mozilla::hwinference::HWInferenceManagerChild::OpenForProcess(
                      std::move(childEp));
                }();
                sHWInferenceChild = mozilla::hwinference::
                    HWInferenceManagerChild::GetSingleton();

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

  // Hold the shared IPC thread for the whole operation, releasing when the
  // promise settles.
  AcquireIPCThreadUser();
  promise->AppendNativeHandler(new IPCThreadUserReleaser());

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
