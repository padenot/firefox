/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionBackend.h"

#include <speex/speex_resampler.h>

#include <algorithm>

#include "AudioConfig.h"
#include "AudioConverter.h"
#include "CubebUtils.h"
#include "SpeechRecognition.h"
#include "VideoUtils.h"
#include "fmt/format.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/ipc/HWInferenceManagerChild.h"
#include "mozilla/ipc/SpeechRecognitionChild.h"
#include "nsComponentManagerUtils.h"
#include "nsGkAtoms.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"

namespace mozilla::dom {

// Static member initialization
StaticRefPtr<nsIThread> SpeechRecognitionBackend::sIPCThread;
StaticRefPtr<mozilla::ipc::HWInferenceManagerChild>
    SpeechRecognitionBackend::sHWInferenceChild;

NS_IMPL_ISUPPORTS0(SpeechRecognitionBackend)

static LazyLogModule gSpeechRecognitionBackendLog("SpeechRecognitionBackend");

#undef LOG
#undef LOGV
#undef LOGE

#define LOG(fmt, ...)                                                      \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Debug, fmt, \
              ##__VA_ARGS__)
#define LOGV(fmt, ...)                                                       \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Verbose, fmt, \
              ##__VA_ARGS__)
#define LOGE(fmt, ...)                                                     \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Error, fmt, \
              ##__VA_ARGS__)

SpeechRecognitionBackend::SpeechRecognitionBackend(
    SpeechRecognition* aParent, uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases)
    : mParent(aParent),
      mLanguage(NS_ConvertUTF16toUTF8(aLanguage).get()),
      mPhrases(aPhrases.Clone()),
      mRingBuffer(MakeUnique<SPSCQueue<float>>(aGraphRate)),  // 1s
      mMonoBuffer(512),
      mGraphRate(aGraphRate) {
  LOG("SpeechRecognitionBackend::SpeechRecognitionBackend, {}, context: {} "
      "phrases, "
      "capture rate: {}",
      mLanguage, mPhrases.Length(), aGraphRate);
}

SpeechRecognitionBackend::~SpeechRecognitionBackend() {
  LOG("SpeechRecognitionBackend::~SpeechRecognitionBackend");
  if (mResamplingThread) {
    Stop();
  }
}

nsresult SpeechRecognitionBackend::Start(uint64_t aSessionId) {
  MOZ_ASSERT(NS_IsMainThread(), "Start must be called on main thread");
  LOG("SpeechRecognitionBackend::Start - session ID: {}", aSessionId);

  mSessionId = aSessionId;

  MOZ_ASSERT(!mSpeechRecognitionChild);

  // Ensure IPC connection is established, create an IPC session, then start our
  // resampling thread that will feed the IPC real-time audio data at the
  // correcte rate
  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self = RefPtr{this}, aSessionId](bool aSuccess) {
        if (!aSuccess) {
          LOGE("Failed to establish IPC connection in Start()");
          return;
        }
        // Start the background thread
        self->mResamplingThreadRunning.store(true, std::memory_order_release);
        nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
            "SpeechRecognitionBackend::ProcessAudioOnBackgroundThread",
            [self]() { self->StartProcessingAudioOnBackgroundThread(); });
        nsresult rv = NS_NewNamedThread("SpeechResampler",
                                        getter_AddRefs(self->mResamplingThread),
                                        runnable.forget());
        if (NS_FAILED(rv)) {
          LOGE("Failed to create background thread: {:x}",
               static_cast<uint32_t>(rv));
          self->mResamplingThreadRunning.store(false,
                                               std::memory_order_release);
        }
        OnIPCThread([self, aSessionId]() {
          self->StartSpeechRecognitionSession(aSessionId, self->mLanguage);
        });
      },
      [self = RefPtr{this}](nsresult aError) {
        LOGE("IPC connection failed in Start(): {:x}",
             static_cast<uint32_t>(aError));
      });

  return NS_OK;
}

void SpeechRecognitionBackend::Stop() {
  LOG("SpeechRecognitionBackend::Stop ({})", mSessionId.load());

  if (mSessionId != 0 && mResamplingThread) {
    RefPtr<SpeechRecognitionBackend> self = this;
    uint64_t sessionId = mSessionId;
    OnIPCThread([self = RefPtr{this}, sessionId]() {
      self->StopSpeechRecognitionSession(sessionId);
    });
    mResamplingThreadRunning.store(false, std::memory_order_release);
    mResamplingThread->Shutdown();
    mResamplingThread = nullptr;
    mSessionId = 0;
  }

  mMonoBuffer.Clear();
}

void SpeechRecognitionBackend::Abort() {
  LOG("SpeechRecognitionBackend::Abort");
  Stop();
  // Cleanup more things...
}

void SpeechRecognitionBackend::DataCallback(TrackTime aTime,
                                            const AudioChunk& aChunk) {
  MOZ_ASSERT(!NS_IsMainThread(), "DataCallback must be on graph thread");

  if (aChunk.IsNull() || aChunk.mDuration == 0) {
    LOG("Null chunk in SpeechRecognitionBackend::DataCallback");
    return;
  }

  size_t frameCount = static_cast<size_t>(aChunk.mDuration);

  // Make sure our pre-allocated buffer is large enough
  // while loop + chunk
  if (mMonoBuffer.Capacity() < frameCount) {
    LOGE("Warning: chunk size {} exceeds pre-allocated buffer capacity {}",
         frameCount, mMonoBuffer.Capacity());
    // In production we might want to handle this more gracefully,
    // but for now just skip this chunk to avoid allocation
    return;
  }

  // Set the length without allocating (we have the capacity)
  mMonoBuffer.SetLengthAndRetainStorage(frameCount);

  // Get a span for the output buffer
  AudioDataValue* monoData = mMonoBuffer.Elements();
  Span<AudioDataValue* const> outputChannels(&monoData, 1);

  // Downmix to mono
  aChunk.DownMixTo(outputChannels);

  // Push the mono audio data to the ring buffer
  // The ring buffer expects float, and AudioDataValue is float on desktop
  // platforms
  LOGV("Pushing {} frames to ring buffer", frameCount);
  int written = mRingBuffer->Enqueue(mMonoBuffer.Elements(),
                                     AssertedCast<int>(frameCount));

  if (written < static_cast<int>(frameCount)) {
    LOG("Ring buffer overflow: wrote {} of {} frames", written, frameCount);
  }
}

void SpeechRecognitionBackend::StartProcessingAudioOnBackgroundThread() {
  AssertOnResamplingThread();

  // Start the audio processing runnable
  ProcessAudioChunk();
}

// [Background thread] Process a single chunk of audio data
// Resamples from graph rate to 16kHz and sends to HWInference
void SpeechRecognitionBackend::ProcessAudioChunk() {
  AssertOnResamplingThread();
  if (!mResamplingThreadRunning.load(std::memory_order_acquire)) {
    LOG("Background thread stopping, not scheduling next audio chunk");
    return;
  }

  // Target sample rate for ASR models
  const int32_t kTargetRate = 16000;

  if (!mAudioConverter) {
    // Setup AudioConverter for resampling from graph rate to 16kHz
    AudioConfig inputConfig(1, mGraphRate, AudioConfig::FORMAT_FLT);
    AudioConfig outputConfig(1, kTargetRate, AudioConfig::FORMAT_FLT);
    mAudioConverter = MakeUnique<AudioConverter>(inputConfig, outputConfig,
                                                 SPEEX_RESAMPLER_QUALITY_MIN);
  }

  int available = mRingBuffer->AvailableRead();
  double secondsAvailable = AssertedCast<double>(available) / mGraphRate;
  const double IPC_BLOCK_SIZE_S = 0.5;

  bool flushed = false;
  // Only send data if enough has been accumulated.
  if (secondsAvailable > IPC_BLOCK_SIZE_S) {
    flushed = true;
    nsTArray<float> audioBuffer;
    audioBuffer.SetLength(available);
    int read = mRingBuffer->Dequeue(audioBuffer.Elements(), available);

    // Resample to 16kHz
    nsTArray<float> resampledBuffer;
    mAudioConverter->Process(resampledBuffer, audioBuffer.Elements(), read);

    size_t frames = resampledBuffer.Length();

    LOGV("Sending {}s of audio via IPC", frames / kTargetRate);
    SendAudioDataViaIPC(mSessionId, std::move(resampledBuffer));
  } else {
    LOGV("Not enough data in ringbuffer ({}s), retrying in a bit",
         secondsAvailable);
  }

  // Schedule next processing in about 500ms
  nsCOMPtr<nsIRunnable> nextChunk = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::ProcessAudioChunk",
      [self = RefPtr{this}]() { self->ProcessAudioChunk(); });

  uint32_t nextProcessingTime =
      flushed ? AssertedCast<uint32_t>(IPC_BLOCK_SIZE_S * 1000) : 100;
  mResamplingThread->DelayedDispatch(nextChunk.forget(), nextProcessingTime);
}

void SpeechRecognitionBackend::StartSpeechRecognitionSession(
    uint64_t aSessionId, const nsCString& aLanguage) {
  AssertOnIPCThread();

  mSpeechRecognitionChild =
      sHWInferenceChild->CreateSpeechRecognitionSession(aSessionId);
  // Set up callbacks for receiving results
  // Capture weak reference to avoid circular reference
  mSpeechRecognitionChild->SetResultCallback(
      [self = RefPtr{this}, aSessionId](const nsCString& aTranscript,
                                        bool aIsFinal) {
        // Handle speech recognition result
        LOG("Received recognition result for session {}: {} (final={})",
            aSessionId, aTranscript.get(), aIsFinal);

        self->HandleRecognitionResult(aTranscript, aIsFinal);
      });

  mSpeechRecognitionChild->SetErrorCallback(
      [self = RefPtr{this}, aSessionId](const nsCString& aError) {
        // Handle speech recognition error
        LOGE("Recognition error for session {}: {}", aSessionId, aError.get());

        self->HandleRecognitionError(aError);
      });

  // Initialize the session with the language and biasing phrases
  mSpeechRecognitionChild->SendInit(aLanguage, mPhrases);
}

void SpeechRecognitionBackend::SendAudioDataViaIPC(uint64_t aSessionId,
                                                   nsTArray<float>&& aAudioData) {
  AssertOnResamplingThread();

  // Dispatch the actual IPC call to the IPC thread
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

void SpeechRecognitionBackend::StopSpeechRecognitionSession(
    uint64_t aSessionId) {
  AssertOnIPCThread();
  LOG("Stopping HWInference speech recognition session: {}", aSessionId);
  mSpeechRecognitionChild->SendStop();
  // Clean up the actor
  mozilla::ipc::SpeechRecognitionChild::Send__delete__(mSpeechRecognitionChild);
  mSpeechRecognitionChild = nullptr;
}

// [Background thread] Handle recognition result from IPC
// Dispatches to main thread via SpeechRecognition parent
void SpeechRecognitionBackend::HandleRecognitionResult(
    const nsCString& aTranscript, bool aIsFinal) {
  MOZ_ASSERT(!NS_IsMainThread(), "Called from background thread");
  LOG("HandleRecognitionResult: {} (final={})", aTranscript.get(), aIsFinal);

  // Dispatch to main thread to call parent's method
  RefPtr<SpeechRecognition> parent = mParent;
  nsCOMPtr<nsIRunnable> resultRunnable = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::HandleRecognitionResult",
      [parent, transcript = nsCString(aTranscript), aIsFinal]() {
        parent->HandleRecognitionResultFromBackend(transcript, aIsFinal);
      });
  NS_DispatchToMainThread(resultRunnable.forget());
}

// [Background thread] Handle recognition error from IPC
// Dispatches to main thread via SpeechRecognition parent
void SpeechRecognitionBackend::HandleRecognitionError(const nsCString& aError) {
  MOZ_ASSERT(!NS_IsMainThread(), "Called from background thread");
  LOGE("HandleRecognitionError: {}", aError.get());

  // Dispatch to main thread to call parent's method
  RefPtr<SpeechRecognition> parent = mParent;
  nsCOMPtr<nsIRunnable> errorRunnable =
      NS_NewRunnableFunction("SpeechRecognitionBackend::HandleRecognitionError",
                             [parent, error = nsCString(aError)]() {
                               parent->HandleRecognitionErrorFromBackend(error);
                             });
  NS_DispatchToMainThread(errorRunnable.forget());
}

/* static */
RefPtr<nsIThread> SpeechRecognitionBackend::GetOrCreateIPCThread() {
  MOZ_ASSERT(NS_IsMainThread(),
             "GetOrCreateIPCThread must be called on main thread");

  if (!sIPCThread) {
    nsCOMPtr<nsIThread> thread;
    nsresult rv = NS_NewNamedThread("SpeechIPC", getter_AddRefs(thread));
    if (NS_SUCCEEDED(rv)) {
      sIPCThread = thread;
      LOG("Created shared IPC thread for speech recognition");
    } else {
      LOG("Failed to create shared IPC thread");
      return nullptr;
    }
  }

  return sIPCThread;
}

/* static */
void SpeechRecognitionBackend::AssertOnIPCThread() {
  MOZ_ASSERT(sIPCThread->IsOnCurrentThread(),
             "Must be called on shared IPC thread");
}

void SpeechRecognitionBackend::AssertOnResamplingThread() {
  MOZ_ASSERT(mResamplingThread->IsOnCurrentThread(),
             "Must be called on resampling thread");
}

/* static */
template <typename Func>
void SpeechRecognitionBackend::OnIPCThread(Func&& aFunc) {
  GetOrCreateIPCThread()->Dispatch(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::OnIPCThread",
      [func = std::forward<Func>(aFunc)]() mutable { func(); }));
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Available(
    nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages) {
  MOZ_ASSERT(NS_IsMainThread(), "Available must be called on main thread");

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  // Convert languages to UTF-8
  nsTArray<nsCString> languages;
  for (const nsString& lang : aLanguages) {
    languages.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }
  if (languages.IsEmpty()) {
    // Default to en-US if no languages specified
    languages.AppendElement("en-US"_ns);
  }

  LOG("SpeechRecognitionBackend::Available - Starting availability check for "
      "%zu languages",
      languages.Length());

  for (const auto& lang : languages) {
    LOG("SpeechRecognitionBackend::Available - Language requested: %s",
        lang.get());
  }

  // Ensure IPC connection is established, then check availability
  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promise, languages = std::move(languages)](bool aSuccess) mutable {
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Available - Failed to initialize IPC");
          promise->MaybeResolve(
              SpeechRecognitionAvailabilityStatus::Unavailable);
          return;
        }

        // Dispatch the availability check to the shared IPC thread
        OnIPCThread([promise, languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Available - Connection ready, "
              "checking "
              "model availability");

          // Create a temporary speech recognition session to check model
          // availability
          RefPtr<mozilla::ipc::SpeechRecognitionChild> speechChild =
              sHWInferenceChild->CreateSpeechRecognitionSession(PR_Now());

          if (!speechChild) {
            LOG("SpeechRecognitionBackend::Available - Failed to create speech "
                "recognition session");
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::ResolveUnavailable", [promise]() {
                  promise->MaybeResolve(
                      SpeechRecognitionAvailabilityStatus::Unavailable);
                }));
            return;
          }

          speechChild->SendIsModelAvailable(languages)->Then(
              GetCurrentSerialEventTarget(), __func__,
              [promise](bool available) {
                LOG("SpeechRecognitionBackend::Available - Received response: "
                    "%s",
                    available ? "true" : "false");
                // Dispatch result to main thread
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveAvailable",
                    [promise, available]() {
                      if (available) {
                        promise->MaybeResolve(
                            SpeechRecognitionAvailabilityStatus::Available);
                      } else {
                        promise->MaybeResolve(
                            SpeechRecognitionAvailabilityStatus::No_model);
                      }
                    }));
              },
              [promise](mozilla::ipc::ResponseRejectReason reason) {
                LOG("SpeechRecognitionBackend::Available failed: %d",
                    static_cast<int>(reason));
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveUnavailable",
                    [promise]() {
                      promise->MaybeResolve(
                          SpeechRecognitionAvailabilityStatus::Unavailable);
                    }));
              });
        });
      },
      [promise](nsresult aError) {
        LOG("SpeechRecognitionBackend::Available - IPC initialization failed");
        promise->MaybeResolve(SpeechRecognitionAvailabilityStatus::Unavailable);
      });

  return promise.forget();
}

/* static */
RefPtr<mozilla::GenericPromise> SpeechRecognitionBackend::EnsureIPC() {
  MOZ_ASSERT(NS_IsMainThread(), "Must be called on main thread");

  RefPtr<nsIThread> ipcThread = GetOrCreateIPCThread();
  if (!ipcThread) {
    LOG("EnsureIPC - Failed to get IPC thread");
    return mozilla::GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  // Create a promise that will be resolved when connection is ready
  RefPtr<mozilla::GenericPromise::Private> promise =
      new mozilla::GenericPromise::Private(__func__);

  // Check connection status on IPC thread
  OnIPCThread([promise]() {
    if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
      promise->Resolve(true, __func__);
      return;
    }

    LOG("EnsureIPC - No connection, requesting one");

    // Dispatch to main thread to request connection
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "SpeechRecognitionBackend::RequestIPCConnection", [promise]() {
          ContentChild* contentChild = ContentChild::GetSingleton();
          if (!contentChild) {
            LOG("EnsureIPC - No ContentChild available");
            promise->Reject(NS_ERROR_FAILURE, __func__);
            return;
          }

          LOG("EnsureIPC - Requesting HWInference connection");
          contentChild->SendRequestHWInferenceConnection()->Then(
              GetCurrentSerialEventTarget(), __func__,
              [promise](mozilla::ipc::Endpoint<
                        mozilla::ipc::PHWInferenceManagerChild>&& aEndpoint) {
                LOG("EnsureIPC - Got endpoint, opening on IPC thread");

                // Dispatch back to the IPC thread to open the connection
                OnIPCThread(
                    [promise, endpoint = std::move(aEndpoint)]() mutable {
                      // Open the connection on the IPC thread
                      mozilla::ipc::HWInferenceManagerChild::OpenForProcess(
                          std::move(endpoint));

                      sHWInferenceChild =
                          mozilla::ipc::HWInferenceManagerChild::GetSingleton();

                      if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
                        LOG("EnsureIPC - Connection established");
                        promise->Resolve(true, __func__);
                      } else {
                        LOG("EnsureIPC - Failed to establish connection");
                        promise->Reject(NS_ERROR_FAILURE, __func__);
                      }
                    });
              },
              [promise](mozilla::ipc::ResponseRejectReason aReason) {
                LOG("EnsureIPC - Failed to get connection: {}",
                    static_cast<int>(aReason));
                promise->Reject(NS_ERROR_FAILURE, __func__);
              });
        }));
  });

  return promise;
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Install(
    nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages) {
  MOZ_ASSERT(NS_IsMainThread(), "Install must be called on main thread");

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  // TODO: validate language tag...

  if (aLanguages.IsEmpty()) {
    // The promise resolves to false if options.langs is empty [...]
    promise->MaybeResolve(false);
    return promise.forget();
  }

  // Convert languages to UTF-8
  nsTArray<nsCString> languages;
  for (const nsString& lang : aLanguages) {
    languages.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }

  LOG("SpeechRecognitionBackend::Install - Starting install for %zu languages",
      languages.Length());

  // Ensure IPC connection is established, then install models
  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promise, languages = std::move(languages)](bool aSuccess) mutable {
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Install - Failed to initialize IPC");
          promise->MaybeResolve(false);
          return;
        }

        // Dispatch the install to the shared IPC thread
        OnIPCThread([promise, languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Install - Connection ready, starting "
              "model installation");

          // Create a temporary speech recognition session to install models
          RefPtr<mozilla::ipc::SpeechRecognitionChild> speechChild =
              sHWInferenceChild->CreateSpeechRecognitionSession(PR_Now());

          if (!speechChild) {
            LOG("SpeechRecognitionBackend::Install - Failed to create speech "
                "recognition session");
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::ResolveInstallFailed",
                [promise]() { promise->MaybeResolve(false); }));
            return;
          }

          speechChild->SendInstallModels(std::move(languages))
              ->Then(
                  GetCurrentSerialEventTarget(), __func__,
                  [promise](bool success) {
                    LOG("SpeechRecognitionBackend::Install - Install "
                        "completed: %s",
                        success ? "success" : "failed");
                    // Dispatch result to main thread
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstall",
                        [promise, success]() {
                          promise->MaybeResolve(success);
                        }));
                  },
                  [promise](mozilla::ipc::ResponseRejectReason aReason) {
                    LOG("SpeechRecognitionBackend::Install - Install failed "
                        "with "
                        "reason: %d",
                        static_cast<int>(aReason));
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstallFailed",
                        [promise]() { promise->MaybeResolve(false); }));
                  });
        });
      },
      [promise](nsresult aError) {
        LOG("SpeechRecognitionBackend::Install - IPC initialization failed");
        promise->MaybeResolve(false);
      });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOGV
#undef LOGD
#undef LOGE
