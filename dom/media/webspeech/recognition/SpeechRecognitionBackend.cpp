/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionBackend.h"

#include <speex/speex_resampler.h>

#include "AudibilityMonitor.h"
#include "AudioConfig.h"
#include "AudioConverter.h"
#include "MainThreadUtils.h"
#include "SpeechRecognition.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/ipc/HWInferenceManagerChild.h"
#include "mozilla/ipc/SpeechRecognitionChild.h"
#include "nsCOMPtr.h"
#include "nsString.h"

namespace mozilla::dom {

using namespace mozilla::ipc;

StaticRefPtr<nsIThread> SpeechRecognitionBackend::sIPCThread;
mozilla::EventTargetCapability<nsIThread>*
    SpeechRecognitionBackend::sIPCCapability = nullptr;
int SpeechRecognitionBackend::sIPCThreadUsers = 0;
StaticRefPtr<HWInferenceManagerChild>
    SpeechRecognitionBackend::sHWInferenceChild;

NS_IMPL_ISUPPORTS0(SpeechRecognitionBackend)

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

// Approximate duration in seconds, of an audio block sent via IPC
static constexpr double IPC_BLOCK_SIZE_S = 0.5;
// Target sample rate for ASR models
static constexpr int32_t SPEECH_RECOGNITION_TARGET_RATE = 16000;

SpeechRecognitionBackend::SpeechRecognitionBackend(
    SpeechRecognition* aParent, uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases)
    : mParent(aParent),
      mLanguage(NS_ConvertUTF16toUTF8(aLanguage).get()),
      mPhrases(aPhrases.Clone()),
      mRingBuffer(MakeUnique<SPSCQueue<float>>(SPEECH_RECOGNITION_TARGET_RATE *
                                               IPC_BLOCK_SIZE_S * 4)),  // 2s
      mResamplingCapability(nullptr),
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

nsresult SpeechRecognitionBackend::Start() {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Start");

  MOZ_ASSERT(!mSpeechRecognitionChild);

  // Initialize the audibility monitor (500ms silence duration)
  mAudibilityMonitor = MakeUnique<AudibilityMonitor>(mGraphRate, 0.5f);
  mCurrentlyAudible = false;

  // Ensure IPC connection is established, create an IPC session, then start our
  // resampling thread that will feed the IPC real-time audio data at the
  // correcte rate
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

  RefPtr<SpeechRecognition> parent = mParent;

  // If sound was detected, dispatch soundend first (per spec)
  if (mCurrentlyAudible) {
    nsCOMPtr<nsIRunnable> soundendRunnable = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::DispatchSoundEnd",
        [parent]() { parent->DispatchTrustedEvent(u"soundend"_ns); });
    NS_DispatchToMainThread(soundendRunnable.forget());
    mCurrentlyAudible = false;
  }

  // Then dispatch audioend event
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

void SpeechRecognitionBackend::DataCallback(TrackTime aTime,
                                            const AudioChunk& aChunk) {
  MOZ_ASSERT(!NS_IsMainThread(), "DataCallback must be on graph thread");

  if (aChunk.IsNull() || aChunk.mDuration == 0) {
    LOG("Null chunk in SpeechRecognitionBackend::DataCallback");
    return;
  }

  size_t frameCount = static_cast<size_t>(aChunk.mDuration);

  // Make sure our pre-allocated buffer is large enough
  if (mMonoBuffer.Capacity() < frameCount) {
    LOGE("Warning: chunk size {} exceeds pre-allocated buffer capacity {}",
         frameCount, mMonoBuffer.Capacity());
    mMonoBuffer.SetLength(frameCount);
    MOZ_DIAGNOSTIC_CRASH("Implement chunked downmixing");
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
  // LOGV("Pushing {} frames to ring buffer", frameCount);
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
  mResamplingCapability.AssertOnCurrentThread();
  if (!mResamplingThreadRunning.load(std::memory_order_acquire)) {
    LOG("Background thread stopping, not scheduling next audio chunk");
    return;
  }

  LOGV("ProcessAudioChunk");

  if (!mAudioConverter) {
    // Setup AudioConverter for resampling from graph rate to 16kHz
    AudioConfig inputConfig(1, mGraphRate, AudioConfig::FORMAT_FLT);
    AudioConfig outputConfig(1, SPEECH_RECOGNITION_TARGET_RATE,
                             AudioConfig::FORMAT_FLT);
    mAudioConverter = MakeUnique<AudioConverter>(inputConfig, outputConfig,
                                                 SPEEX_RESAMPLER_QUALITY_MIN);
  }

  int available = mRingBuffer->AvailableRead();
  double secondsAvailable = AssertedCast<double>(available) / mGraphRate;
  bool flushed = false;
  // Only send data if enough has been accumulated.
  if (secondsAvailable > IPC_BLOCK_SIZE_S) {
    flushed = true;
    nsTArray<float> audioBuffer;
    audioBuffer.SetLength(available);
    int read = mRingBuffer->Dequeue(audioBuffer.Elements(), available);

    // Dispatch audiostart event on first audio processing
    if (!mAudioStartDispatched) {
      mAudioStartDispatched = true;
      RefPtr<SpeechRecognition> parent = mParent;
      nsCOMPtr<nsIRunnable> audiostartRunnable = NS_NewRunnableFunction(
          "SpeechRecognitionBackend::DispatchAudioStart",
          [parent]() { parent->DispatchTrustedEvent(u"audiostart"_ns); });
      NS_DispatchToMainThread(audiostartRunnable.forget());
    }

    // Check audibility after dequeuing (mono audio, so 1 channel)
    if (mAudibilityMonitor) {
      const float* audioData = audioBuffer.Elements();
      mAudibilityMonitor->ProcessPlanar(Span<const float* const>(&audioData, 1),
                                        read);

      bool nowAudible = mAudibilityMonitor->RecentlyAudible();
      if (nowAudible != mCurrentlyAudible) {
        // Audibility changed, dispatch appropriate event
        mCurrentlyAudible = nowAudible;

        RefPtr<SpeechRecognition> parent = mParent;
        nsString eventName = nowAudible ? u"soundstart"_ns : u"soundend"_ns;
        nsCOMPtr<nsIRunnable> soundEventRunnable = NS_NewRunnableFunction(
            "SpeechRecognitionBackend::DispatchSoundEvent",
            [parent, eventName]() { parent->DispatchTrustedEvent(eventName); });
        NS_DispatchToMainThread(soundEventRunnable.forget());
      }
    }

    // Resample to 16kHz
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

  // Schedule next processing in about 500ms
  nsCOMPtr<nsIRunnable> nextChunk = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::ProcessAudioChunk", [self = RefPtr{this}]() {
        self->AssertOnResamplingThread();
        self->ProcessAudioChunk();
      });

  uint32_t nextProcessingTime =
      flushed ? AssertedCast<uint32_t>(IPC_BLOCK_SIZE_S * 1000) : 100;
  mResamplingThread->DelayedDispatch(nextChunk.forget(), nextProcessingTime);
}

void SpeechRecognitionBackend::StartSpeechRecognitionSession(
    const nsCString& aLanguage) {
  AssertOnIPCThread();

  mSpeechRecognitionChild = sHWInferenceChild->CreateSpeechRecognitionSession();
  // Set up callbacks for receiving results
  // Capture weak reference to avoid circular reference
  mSpeechRecognitionChild->SetResultCallback(
      [self = RefPtr{this}](const nsCString& aTranscript, bool aIsFinal) {
        AssertOnIPCThread();
        // Handle speech recognition result
        LOG("Received recognition result: {} (final={})", aTranscript.get(),
            aIsFinal);

        self->HandleRecognitionResult(aTranscript, aIsFinal);
      });

  mSpeechRecognitionChild->SetErrorCallback(
      [self = RefPtr{this}](const nsCString& aError) {
        AssertOnIPCThread();
        // Handle speech recognition error
        LOGE("Recognition error: {}", aError.get());

        self->HandleRecognitionError(aError);
      });

  mSpeechRecognitionChild->SetSpeechChangeCallback(
      [self = RefPtr{this}](bool aSpeechDetected) {
        // Handle speech change events from HWInference process
        LOG("Speech change: {}", aSpeechDetected ? "started" : "ended");

        // Dispatch speechstart/speechend events to main thread
        RefPtr<SpeechRecognition> parent = self->mParent;
        nsCOMPtr<nsIRunnable> eventRunnable = NS_NewRunnableFunction(
            "SpeechRecognitionBackend::HandleSpeechChange",
            [parent, speechDetected = aSpeechDetected]() {
              parent->DispatchTrustedEvent(speechDetected ? u"speechstart"_ns
                                                          : u"speechend"_ns);
            });
        NS_DispatchToMainThread(eventRunnable.forget());
      });

  // Initialize the session with the language and biasing phrases
  mSpeechRecognitionChild->SendInit(aLanguage, mPhrases)
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
              // Start the background thread
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
                // Initialize the EventTargetCapability for thread safety
                // analysis
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

void SpeechRecognitionBackend::SendAudioDataViaIPC(
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

void SpeechRecognitionBackend::StopSpeechRecognitionSession() {
  AssertOnIPCThread();
  LOG("Stopping HWInference speech recognition session");
  mSpeechRecognitionChild->SendStop();
  // Clean up the actor
  SpeechRecognitionChild::Send__delete__(mSpeechRecognitionChild);
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
nsCOMPtr<nsIThread> SpeechRecognitionBackend::GetOrCreateIPCThread() {
  AssertIsOnMainThread();

  if (!sIPCThread) {
    nsCOMPtr<nsIThread> thread;
    nsresult rv = NS_NewNamedThread("SpeechIPC", getter_AddRefs(thread));
    if (NS_SUCCEEDED(rv)) {
      sIPCThread = thread;
      // Create the EventTargetCapability for static analysis
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
  if (sIPCThreadUsers == 0) {
    nsCOMPtr<nsIThread> ipcThread = sIPCThread.forget();
    ipcThread->Shutdown();
    delete sIPCCapability;
    sIPCCapability = nullptr;
    LOG("Stopped shared IPC thread");
  }
}

/* static */
void SpeechRecognitionBackend::AssertOnIPCThread() {
  sIPCCapability->AssertOnCurrentThread();
}

void SpeechRecognitionBackend::AssertOnResamplingThread() {
  MOZ_ASSERT(mResamplingThread->IsOnCurrentThread(),
             "Must be called on resampling thread");
}

/* static */
template <typename Func>
void SpeechRecognitionBackend::OnIPCThread(Func&& aFunc) {
  MOZ_ASSERT(sIPCThread, "Programming error: IPC thread not initialized");
  sIPCThread->Dispatch(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::OnIPCThread", std::forward<Func>(aFunc)));
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
      "{} languages",
      languages.Length());

  for (const auto& lang : languages) {
    LOG("SpeechRecognitionBackend::Available - Language requested: {}",
        lang.get());
  }

  sIPCThreadUsers++;

  // Ensure IPC connection is established, then check availability
  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promise, languages = std::move(languages)](bool aSuccess) mutable {
        AssertIsOnMainThread();
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Available - Failed to initialize IPC");
          promise->MaybeResolve(AvailabilityStatus::Unavailable);
          return;
        }

        // Dispatch the availability check to the shared IPC thread
        OnIPCThread([promise, languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Available - Connection ready, "
              "checking model availability");

          // Create a temporary speech recognition session to check model
          // availability
          RefPtr<SpeechRecognitionChild> speechChild =
              sHWInferenceChild->CreateSpeechRecognitionSession();

          if (!speechChild) {
            LOG("SpeechRecognitionBackend::Available - Failed to create speech "
                "recognition session");
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::ResolveUnavailable", [promise]() {
                  sIPCThreadUsers--;
                  StopIPCThreadIfPossible();
                  promise->MaybeResolve(AvailabilityStatus::Unavailable);
                }));
            return;
          }

          // Careful to pass a reference to the child in the lambdas here to
          // keep it alive. Ensure we explicitly delete the temporary actor
          // after the request completes to avoid actor accumulation.
          speechChild->SendIsModelAvailable(languages)->Then(
              GetCurrentSerialEventTarget(), __func__,
              [promise, speechChild = RefPtr{speechChild}](bool available) {
                LOG("SpeechRecognitionBackend::Available - Received response: "
                    "{}",
                    available ? "true" : "false");
                // Delete the temporary actor on the IPC thread
                SpeechRecognitionChild::Send__delete__(speechChild);
                // Dispatch result to main thread
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveAvailable",
                    [promise, available]() {
                      if (available) {
                        promise->MaybeResolve(AvailabilityStatus::Available);
                      } else {
                        // Model not available but can be downloaded
                        promise->MaybeResolve(AvailabilityStatus::Downloadable);
                      }
                      sIPCThreadUsers--;
                      StopIPCThreadIfPossible();
                    }));
              },
              [promise,
               speechChild = RefPtr{speechChild}](ResponseRejectReason reason) {
                LOG("SpeechRecognitionBackend::Available failed: {}",
                    static_cast<int>(reason));
                // Delete the temporary actor on the IPC thread
                SpeechRecognitionChild::Send__delete__(speechChild);
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveUnavailable",
                    [promise]() {
                      promise->MaybeResolve(AvailabilityStatus::Unavailable);
                      sIPCThreadUsers--;
                      StopIPCThreadIfPossible();
                    }));
              });
        });
      },
      [promise](nsresult aError) {
        LOG("SpeechRecognitionBackend::Available - IPC initialization failed");
        sIPCThreadUsers--;
        StopIPCThreadIfPossible();
        promise->MaybeResolve(AvailabilityStatus::Unavailable);
      });

  return promise.forget();
}

/* static */
RefPtr<GenericPromise> SpeechRecognitionBackend::EnsureIPC() {
  AssertIsOnMainThread();

  nsCOMPtr<nsIThread> ipcThread = GetOrCreateIPCThread();
  if (!ipcThread) {
    LOG("EnsureIPC - Failed to get IPC thread");
    return mozilla::GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  return InvokeAsync(ipcThread, __func__, []() -> RefPtr<GenericPromise> {
    if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
      return GenericPromise::CreateAndResolve(true, __func__);
    }

    LOG("EnsureIPC - No connection, requesting one");

    // Need to go back to main thread to request connection
    return InvokeAsync(
        GetMainThreadSerialEventTarget(), __func__,
        []() -> RefPtr<GenericPromise> {
          ContentChild* contentChild = ContentChild::GetSingleton();
          AssertIsOnMainThread();
          if (!contentChild) {
            LOG("EnsureIPC - No ContentChild available");
            return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
          }

          LOG("EnsureIPC - Creating endpoint pair");

          // Create the endpoint pair locally
          Endpoint<PHWInferenceManagerParent> parentEp;
          Endpoint<PHWInferenceManagerChild> childEp;

          MOZ_ALWAYS_SUCCEEDS(
              PHWInferenceManager::CreateEndpoints(&parentEp, &childEp));
          DebugOnly ok = contentChild->SendRequestHWInferenceConnection(
              std::move(parentEp));
          MOZ_ASSERT(ok);

          // Now go back to IPC thread to open the connection
          return InvokeAsync(
              sIPCThread, __func__,
              [endpoint =
                   std::move(childEp)]() mutable -> RefPtr<GenericPromise> {
                AssertOnIPCThread();
                // Open the connection on the IPC thread
                HWInferenceManagerChild::OpenForProcess(std::move(endpoint));

                sHWInferenceChild = HWInferenceManagerChild::GetSingleton();

                if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
                  LOG("EnsureIPC - Connection established");
                  return GenericPromise::CreateAndResolve(true, __func__);
                }
                LOG("EnsureIPC - Failed to establish connection");
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

  // TODO: validate language tag
  // https://bugzilla.mozilla.org/show_bug.cgi?id=2002306

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

  sIPCThreadUsers++;

  LOG("SpeechRecognitionBackend::Install - Starting install for {} languages",
      languages.Length());

  // Ensure IPC connection is established, then install models
  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promise, languages = std::move(languages)](bool aSuccess) mutable {
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Install - Failed to initialize IPC");
          sIPCThreadUsers--;
          StopIPCThreadIfPossible();
          promise->MaybeResolve(false);
          return;
        }

        // Dispatch the install to the shared IPC thread
        OnIPCThread([promise, languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Install - Connection ready, starting "
              "model installation");

          // Create a temporary speech recognition session to install models
          RefPtr<SpeechRecognitionChild> speechChild =
              sHWInferenceChild->CreateSpeechRecognitionSession();

          if (!speechChild) {
            LOG("SpeechRecognitionBackend::Install - Failed to create speech "
                "recognition session");
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::ResolveInstallFailed", [promise]() {
                  sIPCThreadUsers--;
                  StopIPCThreadIfPossible();
                  promise->MaybeResolve(false);
                }));
            return;
          }

          // Careful to pass the speechChild in the lambda here to keep it alive
          // and explicitly delete the temporary actor when done
          speechChild->SendInstallModels(std::move(languages))
              ->Then(
                  GetCurrentSerialEventTarget(), __func__,
                  [promise,
                   speechChild = std::move(speechChild)](bool success) {
                    LOG("SpeechRecognitionBackend::Install - Install "
                        "completed: {}",
                        success ? "success" : "failed");
                    // Delete the temporary actor on the IPC thread
                    SpeechRecognitionChild::Send__delete__(speechChild);
                    // Dispatch result to main thread
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstall",
                        [promise, success]() {
                          sIPCThreadUsers--;
                          StopIPCThreadIfPossible();
                          promise->MaybeResolve(success);
                        }));
                  },
                  [promise, speechChild = RefPtr{speechChild}](
                      ResponseRejectReason aReason) {
                    LOG("SpeechRecognitionBackend::Install - Install failed "
                        "with reason: {}",
                        static_cast<int>(aReason));
                    // Delete the temporary actor on the IPC thread
                    SpeechRecognitionChild::Send__delete__(speechChild);
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstallFailed",
                        [promise]() {
                          sIPCThreadUsers--;
                          StopIPCThreadIfPossible();
                          promise->MaybeResolve(false);
                        }));
                  });
        });
      },
      [promise](nsresult aError) {
        LOG("SpeechRecognitionBackend::Install - IPC initialization failed");
        sIPCThreadUsers--;
        StopIPCThreadIfPossible();
        promise->MaybeResolve(false);
      });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
