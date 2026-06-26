/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2  et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionBackend.h"

#include <speex/speex_resampler.h>

#include <algorithm>
#include <type_traits>
#include <utility>

#include "AudibilityMonitor.h"
#include "AudioConfig.h"
#include "AudioConverter.h"
#include "MainThreadUtils.h"
#include "SpeechRecognition.h"
#include "SpeechTrackListener.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/SpeechRecognitionChild.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Promise.h"
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

/* static */
void SpeechRecognitionBackend::CloseHWInferenceChildIfAny() {
  AssertOnIPCThread();
  if (sHWInferenceChild) {
    sHWInferenceChild->Close();
    sHWInferenceChild = nullptr;
  }
}

NS_IMPL_ISUPPORTS(SpeechRecognitionBackend::ShutdownTask, nsITargetShutdownTask)

void SpeechRecognitionBackend::ShutdownTask::TargetShutdown() {
  SpeechRecognitionBackend::CloseHWInferenceChildIfAny();
}

TransientSpeechRecognitionSession::TransientSpeechRecognitionSession()
    : mChild(SpeechRecognitionBackend::sHWInferenceChild
                 ->CreateSpeechRecognitionSession()) {}

TransientSpeechRecognitionSession::~TransientSpeechRecognitionSession() {
  SpeechRecognitionBackend::AssertOnIPCThread();
  if (mChild) {
    if (mChild->CanSend()) {
      SpeechRecognitionChild::Send__delete__(mChild);
    }
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

SpeechRecognitionBackend::IPCThreadUserGuard::~IPCThreadUserGuard() {
  if (NS_IsMainThread()) {
    SpeechRecognitionBackend::ReleaseIPCThreadUser();
  } else {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "IPCThreadUserGuard::Release",
        [] { SpeechRecognitionBackend::ReleaseIPCThreadUser(); }));
  }
}

// Audio is streamed to the inference process in small blocks to keep end-to-end
// latency low. The block is well under the model's internal chunk (~160 ms), so
// the buffering delay is hidden behind the model's own latency rather than
// adding to it. (Was 0.5 s when the backend drove the batch Parakeet engine's
// offline path.) The ring buffer is sized independently, with generous headroom
// against resampling-thread scheduling jitter.
static constexpr double IPC_BLOCK_SIZE_S = 0.04;
static constexpr double RING_BUFFER_SIZE_S = 0.5;
static constexpr uint32_t STREAMING_POLL_MS = 20;
static constexpr int32_t SPEECH_RECOGNITION_TARGET_RATE = 16000;
static constexpr auto SPEECH_RECOGNITION_ENGINE_ID = "parakeet-cpp"_ns;

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

  // Held for the whole session (released in Stop()/Abort()), so the shared
  // IPC thread can't be idle-closed out from under an active session.
  auto [ready, ipcUserGuard] = EnsureIPC();
  mIPCThreadUserGuard = std::move(ipcUserGuard);

  ready->Then(
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

  // Detach before tearing anything else down: SpeechTrackListener holds a
  // strong RefPtr<SpeechRecognitionBackend> while registered, so leaving it
  // attached keeps this backend (and the track->listener->backend->track
  // cycle) alive and feeding DataCallback() from the graph thread after the
  // session has nominally stopped.
  DetachFromTrack();

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
    // Shutdown() blocks by spinning a nested event loop, which is unsafe from
    // e.g. global teardown; AsyncShutdown() just requests it, and
    // mResamplingThreadRunning stops the thread rescheduling itself.
    mResamplingThreadRunning.store(false, std::memory_order_release);
    mResamplingThread->AsyncShutdown();
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

  mResamplingThread->DelayedDispatch(nextChunk.forget(), STREAMING_POLL_MS);
}

void SpeechRecognitionBackend::SendAudioDataViaIPC(
    nsTArray<float>&& aAudioData) {
  AssertOnResamplingThread();

  RefPtr<SpeechRecognitionBackend> self = this;
  OnIPCThread([self, audioData = std::move(aAudioData)]() mutable {
    if (self->mSpeechRecognitionChild &&
        self->mSpeechRecognitionChild->CanSend()) {
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
  if (!mSpeechRecognitionChild) {
    LOGE("Failed to create speech recognition session");
    HandleRecognitionError(nsCString("network"));
    return;
  }

  mSpeechRecognitionChild->SetResultCallback(
      [self = RefPtr{this}](const nsCString& aTranscript, bool aIsFinal,
                            float aConfidence) {
        AssertOnIPCThread();
        LOG("Received recognition result: {} (final={})", aTranscript.get(),
            aIsFinal);

        self->HandleRecognitionResult(aTranscript, aIsFinal, aConfidence);
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

  // The actor can be torn down from the other side (utility process crash,
  // channel close) without Stop()/Abort() ever running. Drop the reference
  // then, instead of leaving mSpeechRecognitionChild pointing at a dead actor
  // that the resampling loop keeps trying to send audio through.
  mSpeechRecognitionChild->SetDestroyedCallback(
      [self = RefPtr{this}, child = mSpeechRecognitionChild]() {
        AssertOnIPCThread();
        if (self->mSpeechRecognitionChild == child) {
          self->mSpeechRecognitionChild = nullptr;
        }
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
              self->DispatchToParentIfAlive(
                  "SpeechRecognitionBackend::NotifyBackendListening",
                  [](SpeechRecognition* aParent) {
                    aParent->NotifyBackendListening();
                  });
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
  if (mSpeechRecognitionChild->CanSend()) {
    mSpeechRecognitionChild->SendStop();
    SpeechRecognitionChild::Send__delete__(mSpeechRecognitionChild);
  }
  mSpeechRecognitionChild = nullptr;
}

void SpeechRecognitionBackend::HandleRecognitionResult(
    const nsCString& aTranscript, bool aIsFinal, float aConfidence) {
  MOZ_ASSERT(!NS_IsMainThread(), "Called from background thread");
  LOG("HandleRecognitionResult: {} (final={})", aTranscript.get(), aIsFinal);

  DispatchToParentIfAlive("SpeechRecognitionBackend::HandleRecognitionResult",
                          [transcript = nsCString(aTranscript), aIsFinal,
                           aConfidence](SpeechRecognition* aParent) {
                            aParent->HandleRecognitionResultFromBackend(
                                transcript, aIsFinal, aConfidence);
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
      // The thread and the HWInference connection it carries are kept alive
      // for the lifetime of the content process (see StopIPCThreadIfPossible)
      // rather than being torn down and recreated opportunistically. Actors
      // are bound to the specific nsIThread instance they were opened on;
      // tearing down and recreating this thread while an actor referencing
      // the old instance could still be in use (e.g. a concurrent EnsureIPC()
      // caller) led to actors being used from the wrong thread and crashing.
      //
      // The actor is closed via a shutdown task registered on the thread
      // itself, which runs on the IPC thread as part of its own shutdown
      // sequence, rather than a runnable dispatched from RunOnShutdown:
      // dispatching to a thread whose queue is already closing can fail, and
      // Gecko's infallible-dispatch failure path (MaybeLeakRefPtr) leaks the
      // runnable instead of running it, silently dropping the close.
      RefPtr<ShutdownTask> shutdownTask = MakeRefPtr<ShutdownTask>();
      MOZ_ALWAYS_SUCCEEDS(sIPCThread->RegisterShutdownTask(shutdownTask));

      RunOnShutdown([] {
        AssertIsOnMainThread();
        if (sIPCThread) {
          nsCOMPtr<nsIThread> thread = sIPCThread.forget();
          delete sIPCCapability;
          sIPCCapability = nullptr;
          thread->Shutdown();
        }
      });
    } else {
      LOG("Failed to create shared IPC thread");
      return nullptr;
    }
  }

  return nsCOMPtr<nsIThread>(sIPCThread);
}

void SpeechRecognitionBackend::StopIPCThreadIfPossible() {
  AssertIsOnMainThread();
  // The shared IPC thread and its HWInference connection are kept alive for
  // the process lifetime (see GetOrCreateIPCThread); nothing to do here once
  // there are no more users. Close the actor when idle so the parent-side
  // connection doesn't linger, but keep the same thread instance around so a
  // later EnsureIPC() call never has to reason about a stale actor bound to a
  // different, dead thread.
  if (sIPCThreadUsers.IsZero() && sIPCThread) {
    // Unlike GetOrCreateIPCThread's ShutdownTask, this doesn't run as part of
    // the thread's own shutdown - it's a live idle-close while the thread
    // keeps running - so it's still a plain dispatch. Pass NS_DISPATCH_FALLIBLE
    // so a Dispatch() that loses a race against real process shutdown releases
    // the runnable normally instead of MaybeLeakRefPtr leaking it; ShutdownTask
    // will close the actor for real once the thread does shut down.
    sIPCThread->Dispatch(
        NS_NewRunnableFunction(
            "SpeechRecognitionBackend::CloseHWInferenceChildWhenIdle",
            [] { CloseHWInferenceChildIfAny(); }),
        NS_DISPATCH_FALLIBLE);
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
        // Also drop this if a newer backend has since replaced self on the
        // parent (e.g. stop() immediately followed by start()): self's
        // callback was already in flight when it was superseded, and its
        // notification belongs to the session it was created for, not
        // whichever one happens to be current by the time this runs.
        if (!parent || !parent->IsCurrentBackend(self.get())) {
          return;
        }
        aFunc(parent.get());
      }));
}

/* static */
template <typename SendFunc, typename OnResult, typename OnFailure>
void SpeechRecognitionBackend::RunWithTransientSession(
    nsTArray<nsCString>&& aLanguages, SendFunc aSendFunc, OnResult aOnResult,
    OnFailure aOnFailure) {
  AssertIsOnMainThread();

  // ipcUserGuard holds the shared IPC thread alive for the whole operation,
  // releasing when the operation's async chain finishes - tied to its own
  // lifetime, not to the promise settling (see IPCThreadUserGuard).
  auto [ready, ipcUserGuard] = EnsureIPC();
  ready->Then(
      GetCurrentSerialEventTarget(), __func__,
      [ipcUserGuard, languages = std::move(aLanguages), aSendFunc, aOnResult,
       aOnFailure](bool aSuccess) mutable {
        AssertIsOnMainThread();
        if (!aSuccess) {
          aOnFailure();
          return;
        }

        OnIPCThread([ipcUserGuard, languages = std::move(languages), aSendFunc,
                     aOnResult, aOnFailure]() mutable {
          auto session = MakeUnique<TransientSpeechRecognitionSession>();
          if (!session->get()) {
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::RunWithTransientSession::"
                "NoSession",
                [ipcUserGuard, aOnFailure]() { aOnFailure(); }));
            return;
          }

          using ResolveValueType = typename std::remove_pointer_t<
              decltype(aSendFunc(session->get(), languages)
                           .get())>::ResolveValueType;
          aSendFunc(session->get(), languages)
              ->Then(
                  GetCurrentSerialEventTarget(), __func__,
                  [ipcUserGuard, session = std::move(session),
                   aOnResult](ResolveValueType aResult) {
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::RunWithTransientSession::"
                        "Resolve",
                        [ipcUserGuard, aOnResult, aResult]() {
                          aOnResult(aResult);
                        }));
                  },
                  [ipcUserGuard, session = std::move(session),
                   aOnFailure](ResponseRejectReason aReason) {
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::RunWithTransientSession::"
                        "Reject",
                        [ipcUserGuard, aOnFailure]() { aOnFailure(); }));
                  });
        });
      },
      [ipcUserGuard, aOnFailure](nsresult aError) { aOnFailure(); });
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

  RunWithTransientSession(
      std::move(languages),
      [](SpeechRecognitionChild* aChild, nsTArray<nsCString>& aLangs) {
        return aChild->SendIsModelAvailable(aLangs);
      },
      [promiseHandle](bool aAvailable) {
        LOG("SpeechRecognitionBackend::Available - Received response: {}",
            aAvailable ? "true" : "false");
        promiseHandle->MaybeResolve(aAvailable
                                        ? AvailabilityStatus::Available
                                        : AvailabilityStatus::Downloadable);
      },
      [promiseHandle]() {
        promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
      });

  return promise.forget();
}

/* static */
SpeechRecognitionBackend::EnsureIPCResult
SpeechRecognitionBackend::EnsureIPC() {
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
    return {
        mozilla::GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__),
        nullptr};
  }

  // Acquired before the first await point so that the thread can't be
  // idle-closed while this connection attempt (or the caller's use of it
  // afterwards) is still in flight.
  RefPtr<IPCThreadUserGuard> ipcUserGuard = new IPCThreadUserGuard();

  RefPtr<GenericPromise> ready =
      InvokeAsync(ipcThread, __func__, []() -> RefPtr<GenericPromise> {
        AssertOnIPCThread();
        if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
          LOG("EnsureIPC - Connection already established (checked on IPC "
              "thread)");
          return GenericPromise::CreateAndResolve(true, __func__);
        }

        return InvokeAsync(
            GetMainThreadSerialEventTarget(), __func__,
            []() -> RefPtr<GenericPromise> {
              AssertIsOnMainThread();

              ContentChild* contentChild = ContentChild::GetSingleton();
              if (!contentChild) {
                LOG("EnsureIPC - No ContentChild available");
                return GenericPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                       __func__);
              }

              LOG("EnsureIPC - Creating endpoint pair");

              mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerParent>
                  parentEp;
              mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerChild>
                  childEp;

              MOZ_ALWAYS_SUCCEEDS(
                  hwinference::PHWInferenceManager::CreateEndpoints(&parentEp,
                                                                    &childEp));

              // Wait for the parent process to confirm the utility process
              // has actually launched and accepted the parent endpoint before
              // opening our side and calling the connection ready: otherwise
              // a launch/accept failure on the other end would go unnoticed
              // and later API calls would fail indirectly instead.
              return contentChild
                  ->SendRequestHWInferenceConnection(std::move(parentEp))
                  ->Then(
                      GetMainThreadSerialEventTarget(), __func__,
                      [childEp = std::move(childEp)](
                          bool aAccepted) mutable -> RefPtr<GenericPromise> {
                        if (!aAccepted) {
                          LOG("EnsureIPC - Utility process failed to accept "
                              "the HWInference connection");
                          return GenericPromise::CreateAndReject(
                              NS_ERROR_FAILURE, __func__);
                        }
                        return InvokeAsync(
                            sIPCThread, __func__,
                            [childEp = std::move(
                                 childEp)]() mutable -> RefPtr<GenericPromise> {
                              AssertOnIPCThread();

                              LOG("EnsureIPC - Opening connection on the "
                                  "SpeechIPC thread");
                              [&]() MOZ_NO_THREAD_SAFETY_ANALYSIS {
                                mozilla::hwinference::HWInferenceManagerChild::
                                    OpenForProcess(std::move(childEp));
                              }();
                              sHWInferenceChild = mozilla::hwinference::
                                  HWInferenceManagerChild::GetSingleton();

                              if (sHWInferenceChild->CanSend()) {
                                LOG("EnsureIPC - Connection established");
                                return GenericPromise::CreateAndResolve(
                                    true, __func__);
                              }
                              LOG("EnsureIPC - Failed to establish");
                              return GenericPromise::CreateAndReject(
                                  NS_ERROR_FAILURE, __func__);
                            });
                      },
                      [](mozilla::ipc::ResponseRejectReason aReason) {
                        LOG("EnsureIPC - RequestHWInferenceConnection IPC "
                            "call failed");
                        return GenericPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                               __func__);
                      });
            });
      });

  return {std::move(ready), std::move(ipcUserGuard)};
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

  RunWithTransientSession(
      std::move(languages),
      [](SpeechRecognitionChild* aChild, nsTArray<nsCString>& aLangs) {
        return aChild->SendInstallModels(std::move(aLangs));
      },
      [promiseHandle](bool aSuccess) {
        LOG("SpeechRecognitionBackend::Install - Install completed: {}",
            aSuccess ? "success" : "failed");
        promiseHandle->MaybeResolve(aSuccess);
      },
      [promiseHandle]() { promiseHandle->MaybeResolve(false); });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
