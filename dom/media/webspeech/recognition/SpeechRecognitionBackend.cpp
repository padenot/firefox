/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2  et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <speex/speex_resampler.h>

#include <type_traits>
#include <utility>

#include "AudibilityMonitor.h"
#include "AudioConfig.h"
#include "AudioConverter.h"
#include "MainThreadUtils.h"
#include "SpeechRecognition.h"
#include "SpeechTrackListener.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/PodOperations.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/hwinference/HWInferenceManagerChild.h"
#include "mozilla/hwinference/SpeechRecognitionChild.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "nsCOMPtr.h"
#include "nsProxyRelease.h"
#include "nsString.h"

namespace mozilla::dom {

using namespace mozilla::ipc;

StaticAutoPtr<mozilla::EventTargetCapability<nsISerialEventTarget>>
    SpeechRecognitionBackend::sIPCCapability;
int32_t SpeechRecognitionBackend::sIPCActorUsers = 0;
StaticRefPtr<nsITimer> SpeechRecognitionBackend::sIdleCloseTimer;

/* static */
void SpeechRecognitionBackend::CloseHWInferenceChildIfAny() {
  AssertOnIPCThread();
  RefPtr<mozilla::hwinference::HWInferenceManagerChild> child =
      mozilla::hwinference::HWInferenceManagerChild::GetSingleton();
  if (child) {
    child->Close();
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

// How long the shared IPC thread stays idle before its backing OS thread is
// released. The serial event target itself lives for the process lifetime.
static constexpr uint32_t IPC_THREAD_IDLE_TIMEOUT_MS = 5000;

/* static */
void SpeechRecognitionBackend::CancelIdleCloseTimer() {
  if (sIdleCloseTimer) {
    sIdleCloseTimer->Cancel();
    sIdleCloseTimer = nullptr;
  }
}

/* static */
void SpeechRecognitionBackend::AcquireIPCActorUser() {
  AssertIsOnMainThread();
  // A new user within the grace period means the connection is wanted again;
  // keep the established one rather than letting the idle close fire.
  CancelIdleCloseTimer();
  sIPCActorUsers++;
}

/* static */
void SpeechRecognitionBackend::ReleaseIPCActorUser() {
  AssertIsOnMainThread();
  MOZ_ASSERT(sIPCActorUsers > 0);
  if (--sIPCActorUsers) {
    return;
  }

  uint32_t graceMs =
      StaticPrefs::media_webspeech_recognition_idle_shutdown_grace_ms();
  // Past shutdown there is nothing left to keep the connection open for, and
  // no shutdown hook left to cancel a timer armed now: close immediately.
  if (!graceMs ||
      AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    CloseIPCActorIfUnused();
    return;
  }

  // The idle timer must not outlive XPCOM: a still-armed timer released from a
  // static destructor crashes in nsTimerImpl::CancelImpl, once the timer
  // thread is gone. Registered here rather than alongside the IPC thread
  // because a keepalive can be acquired and dropped - arming the timer -
  // without any session ever creating that thread.
  static bool sRegisteredShutdownBlocker = false;
  if (!sRegisteredShutdownBlocker) {
    sRegisteredShutdownBlocker = true;
    RunOnShutdown([]() {
      AssertIsOnMainThread();
      CancelIdleCloseTimer();
    });
  }

  LOG("Last HWInference user gone, closing the connection in {}ms", graceMs);
  nsCOMPtr<nsITimer> timer;
  nsresult rv = NS_NewTimerWithCallback(
      getter_AddRefs(timer),
      [](nsITimer*) {
        AssertIsOnMainThread();
        // Only still armed if nothing acquired in the meantime, since
        // AcquireIPCActorUser() cancels the timer.
        sIdleCloseTimer = nullptr;
        CloseIPCActorIfUnused();
      },
      graceMs, nsITimer::TYPE_ONE_SHOT,
      "SpeechRecognitionBackend::IdleClose"_ns);

  if (NS_FAILED(rv)) {
    CloseIPCActorIfUnused();
    return;
  }
  sIdleCloseTimer = timer.forget();
}

/* static */
already_AddRefed<IPCActorUserGuard>
SpeechRecognitionBackend::AcquireProcessKeepAlive() {
  AssertIsOnMainThread();
  return RefPtr<IPCActorUserGuard>(new IPCActorUserGuard()).forget();
}

IPCActorUserGuard::IPCActorUserGuard() {
  SpeechRecognitionBackend::AcquireIPCActorUser();
}

IPCActorUserGuard::~IPCActorUserGuard() {
  if (NS_IsMainThread()) {
    SpeechRecognitionBackend::ReleaseIPCActorUser();
  } else {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "IPCActorUserGuard::Release",
        [] { SpeechRecognitionBackend::ReleaseIPCActorUser(); }));
  }
}

static constexpr double IPC_BLOCK_SIZE_S = 0.5;
static constexpr uint32_t STREAMING_POLL_MS = 20;
static constexpr int32_t SPEECH_RECOGNITION_TARGET_RATE = 16000;
static constexpr auto SPEECH_RECOGNITION_ENGINE_ID = "parakeet-cpp"_ns;
// The ring buffer holds graph-rate audio, not yet downsampled to
// SPEECH_RECOGNITION_TARGET_RATE, with this much headroom for the resampling
// thread being late to dequeue.
static constexpr uint32_t RING_BUFFER_IPC_BLOCKS = 4;
static constexpr uint32_t PER_CALLBACK_MONO_BUFFER_INITIAL_NUM_FRAMES = 512;

/* static */
already_AddRefed<SpeechRecognitionBackend> SpeechRecognitionBackend::Create(
    SpeechRecognition* aParent, uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases) {
  AssertIsOnMainThread();

  // The resampling thread's whole lifetime is owned by the main thread:
  // created here, shut down by Shutdown(). It exists before the backend does
  // so that mResamplingCapability, which has no empty state, is bound to it
  // for the backend's whole lifetime.
  nsCOMPtr<nsIThread> resamplingThread;
  nsresult rv =
      NS_NewNamedThread("SpeechResampler", getter_AddRefs(resamplingThread));
  if (NS_FAILED(rv)) {
    LOGE("Failed to create the resampling thread: {:x}",
         static_cast<uint32_t>(rv));
    return nullptr;
  }

  return RefPtr<SpeechRecognitionBackend>(
             new SpeechRecognitionBackend(aParent, resamplingThread, aGraphRate,
                                          aLanguage, aPhrases))
      .forget();
}

SpeechRecognitionBackend::SpeechRecognitionBackend(
    SpeechRecognition* aParent, nsIThread* aResamplingThread,
    uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases)
    : mParent(aParent),
      mLanguage(NS_ConvertUTF16toUTF8(aLanguage)),
      mPhrases(aPhrases.Clone()),
      mRingBuffer(MakeUnique<SPSCQueue<float>>(AssertedCast<int>(
          aGraphRate * IPC_BLOCK_SIZE_S * RING_BUFFER_IPC_BLOCKS))),
      mResamplingThread(aResamplingThread),
      mResamplingCapability(aResamplingThread),
      mMonoBuffer(PER_CALLBACK_MONO_BUFFER_INITIAL_NUM_FRAMES),
      mGraphRate(aGraphRate) {}

SpeechRecognitionBackend::~SpeechRecognitionBackend() {
  AssertIsOnMainThread();
  MOZ_ASSERT(mStopped, "SpeechRecognition must Stop() or Abort() the backend");
}

void SpeechRecognitionBackend::Start() {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Start");

#ifdef DEBUG
  {
    auto session = mSession.Lock();
    MOZ_ASSERT(!session->mChild);
  }
#endif

  mAudibilityMonitor = MakeUnique<AudibilityMonitor>(mGraphRate, 0.5f);

  // Held for the whole session so the actor cannot be closed while it is in
  // use. The shared IPC thread's serial event target outlives the session; its
  // backing OS thread is released once idle.
  mIPCActorUserGuard = EnsureIPC();

  // Runs after the connection has been opened by EnsureIPC(), the IPC thread
  // being serial.
  nsCOMPtr<nsIRunnable> startSession = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::StartSpeechRecognitionSession",
      [self = RefPtr{this}]() {
        AssertOnIPCThread();
        self->StartSpeechRecognitionSession(self->mLanguage);
      });
  sIPCCapability->Dispatch(startSession.forget());
}

void SpeechRecognitionBackend::Stop() { Shutdown(/* aWaitForFlush */ true); }

void SpeechRecognitionBackend::Abort() {
  LOG("SpeechRecognitionBackend::Abort");
  // https://webaudio.github.io/web-speech-api/#dom-speechrecognition-abort
  // "stop listening and stop recognizing and do not return any information":
  // unlike stop(), no end-of-stream flush to wait for.
  Shutdown(/* aWaitForFlush */ false);
}

// The whole shutdown sequence runs here, on the main thread, in this order:
//
// - disconnect the graph track feeding the ring buffer
// - publish mStopRequested so work already in flight on the other two threads
//   gives up
// - queue the trailing DOM events
// - hand the actor to the IPC thread to stop or abort
// - stop the resampling loop
// - and shut down its thread
//
// The events are queued before the last three steps, not after, so that
// everything they make another thread dispatch to the main thread - a result
// the end-of-stream flush produces, the flush's answer that ends the session -
// is queued behind them, leaving "end" last.
void SpeechRecognitionBackend::Shutdown(bool aWaitForFlush) {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Shutdown waitForFlush={}", aWaitForFlush);

  // Idempotent: JS can call stop() then abort()
  if (mStopped) {
    return;
  }
  mStopped = true;

  // SpeechTrackListener holds a strong RefPtr<SpeechRecognitionBackend> while
  // registered, detaching needs to happen first.
  DetachFromTrack();

  RefPtr<hwinference::SpeechRecognitionChild> childToStop;
  {
    auto session = mSession.Lock();
    session->mStopRequested = true;
    childToStop = std::move(session->mChild);
  }
  MOZ_ASSERT_IF(childToStop, sIPCCapability);
  const bool hadSession = !!childToStop;

  RefPtr<SpeechRecognition> parent(mParent);
  if (parent) {
    if (mCurrentlyAudible) {
      mCurrentlyAudible = false;
      nsCOMPtr<nsIRunnable> soundendRunnable = NS_NewRunnableFunction(
          "SpeechRecognitionBackend::DispatchSoundEnd",
          [parent]() { parent->DispatchTrustedEvent(u"soundend"_ns); });
      NS_DispatchToMainThread(soundendRunnable.forget());
    }

    nsCOMPtr<nsIRunnable> audioendRunnable = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::DispatchAudioEnd",
        [parent]() { parent->DispatchTrustedEvent(u"audioend"_ns); });
    NS_DispatchToMainThread(audioendRunnable.forget());
  }

  if (hadSession && aWaitForFlush) {
    nsCOMPtr<nsIRunnable> stopSession = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::StopSession",
        [self = RefPtr{this}, child = std::move(childToStop)]() {
          AssertOnIPCThread();
          LOG("Stopping HWInference speech recognition session");
          if (!child->CanSend()) {
            self->NotifySessionFinished(/* aProducedResult */ true);
            return;
          }
          child->SendStop()->Then(
              GetCurrentSerialEventTarget(), __func__,
              [self, child](hwinference::PSpeechRecognitionChild::StopPromise::
                                ResolveOrRejectValue&& aValue) {
                hwinference::SpeechRecognitionChild::Send__delete__(child);
                // A dead channel means the engine never reported back, so
                // don't claim a nomatch it never determined.
                self->NotifySessionFinished(aValue.IsReject() ||
                                            aValue.ResolveValue());
              });
        });
    sIPCCapability->Dispatch(stopSession.forget());
  } else if (hadSession) {
    nsCOMPtr<nsIRunnable> abortSession = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::AbortSession",
        [child = std::move(childToStop)]() {
          AssertOnIPCThread();
          LOG("Aborting HWInference speech recognition session");
          if (child->CanSend()) {
            child->SendStop();
            hwinference::SpeechRecognitionChild::Send__delete__(child);
          }
        });
    sIPCCapability->Dispatch(abortSession.forget());
  }

  // Dispatch to the resampling thread to tell it to stop.
  nsCOMPtr<nsIRunnable> stop = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::StopProcessingAudio", [self = RefPtr{this}]() {
        self->mResamplingCapability.AssertOnCurrentThread();
        self->mAudioProcessingStopped = true;
      });
  mResamplingThread->Dispatch(stop.forget());
  // nsIThread::Shutdown() blocks by spinning a nested event loop, which is
  // unsafe from e.g. global teardown; AsyncShutdown() just requests it.
  mResamplingThread->AsyncShutdown();
  mResamplingThread = nullptr;

  if (aWaitForFlush && !hadSession) {
    // Nothing ever reached the engine, so there is no flush to wait for.
    // Queued here rather than earlier so "end" still follows "audioend".
    NotifySessionFinished(/* aProducedResult */ true);
  }
}

void SpeechRecognitionBackend::NotifySessionFinished(bool aProducedResult) {
  DispatchToParentIfAlive("SpeechRecognitionBackend::NotifySessionFinished",
                          [aProducedResult](SpeechRecognition* aParent) {
                            aParent->OnSessionFinished(aProducedResult);
                          });
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

void SpeechRecognitionBackend::DataCallback(MediaTrackGraph* aGraph,
                                            TrackTime aTime,
                                            const AudioChunk& aChunk) {
  aGraph->AssertOnGraphThread();

  if (aChunk.mDuration == 0) {
    return;
  }

  size_t frameCount = static_cast<size_t>(aChunk.mDuration);
  // A null chunk is silence the graph did not bother to materialize, not an
  // absence of audio, so it is fed as zeros rather than dropped. Dropping it
  // would splice together the audio on either side of a silent gap, hiding the
  // silence that ends an utterance from the recognizer.
  const bool isSilence = aChunk.IsNull();

  if (mMonoBuffer.Capacity() < frameCount) {
    LOGE("Warning: chunk size {} exceeds pre-allocated buffer capacity {}",
         frameCount, mMonoBuffer.Capacity());
    mMonoBuffer.SetCapacity(frameCount);
    MOZ_DIAGNOSTIC_CRASH("Implement chunked downmixing");
  }

  mMonoBuffer.SetLengthAndRetainStorage(frameCount);

  AudioDataValue* monoData = mMonoBuffer.Elements();
  Span<AudioDataValue* const> outputChannels(&monoData, 1);

  if (isSilence) {
    PodZero(mMonoBuffer.Elements(), frameCount);
  } else {
    aChunk.DownMixTo(outputChannels);
  }

  int written = mRingBuffer->Enqueue(mMonoBuffer.Elements(),
                                     AssertedCast<int>(frameCount));

  if (written < static_cast<int>(frameCount)) {
    LOG("Ring buffer overflow: wrote {} of {} frames", written, frameCount);
  }
}

void SpeechRecognitionBackend::ProcessAudioChunk() {
  mResamplingCapability.AssertOnCurrentThread();
  // Both "teardown happened while the session was being initialized" and
  // "teardown happened mid-loop" end up here: this is the only thing that
  // stops the loop, and it never goes back to false.
  if (mAudioProcessingStopped) {
    LOG("Resampling loop stopped, not scheduling next audio chunk");
    return;
  }

  LOGV("ProcessAudioChunk, {} frames of graph-rate audio queued",
       mRingBuffer->AvailableRead());

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
      if (nowAudible != mAudible) {
        mAudible = nowAudible;

        // The soundstart/soundend pair is decided on the main thread, which is
        // also where the session ends, rather than here: a transition this
        // thread reports after Shutdown() has already closed the pair is a
        // straggler from a chunk that was in flight, and dropping it is what
        // keeps the two events paired and ahead of "audioend".
        DispatchToParentIfAlive(
            "SpeechRecognitionBackend::DispatchSoundEvent",
            [self = RefPtr{this}, nowAudible](SpeechRecognition* aParent) {
              AssertIsOnMainThread();
              if (self->mStopped || self->mCurrentlyAudible == nowAudible) {
                return;
              }
              self->mCurrentlyAudible = nowAudible;
              aParent->DispatchTrustedEvent(nowAudible ? u"soundstart"_ns
                                                       : u"soundend"_ns);
            });
      }
    }

    nsTArray<float> resampledBuffer;
    size_t frames =
        mAudioConverter->Process(resampledBuffer, audioBuffer.Elements(), read);
    if (!frames) {
      LOGE("AudioConverter::Process failed; dropping this audio chunk");
    } else {
      LOGV("Sending {}s of audio via IPC",
           static_cast<float>(frames) / SPEECH_RECOGNITION_TARGET_RATE);
      SendAudioDataViaIPC(std::move(resampledBuffer));
    }
  } else {
    LOGV("Not enough data in ringbuffer ({}s), retrying in a bit",
         secondsAvailable);
  }

  nsCOMPtr<nsIRunnable> nextChunk = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::ProcessAudioChunk", [self = RefPtr{this}]() {
        self->mResamplingCapability.AssertOnCurrentThread();
        self->ProcessAudioChunk();
      });

  // Rescheduled through the capability, which is this very thread, rather than
  // through mResamplingThread allows us to put a main-thread capability on the
  // thread handle.
  //
  // A failure here means Shutdown() has already asked the thread to go away, so
  // failing to redispatch is what we want.
  mResamplingCapability.GetEventTarget()->DelayedDispatch(nextChunk.forget(),
                                                          STREAMING_POLL_MS);
}

void SpeechRecognitionBackend::SendAudioDataViaIPC(
    nsTArray<float>&& aAudioData) {
  mResamplingCapability.AssertOnCurrentThread();

  nsCOMPtr<nsIRunnable> sendAudio = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::SendAudioData",
      [self = RefPtr{this}, audioData = std::move(aAudioData)]() mutable {
        RefPtr<hwinference::SpeechRecognitionChild> child;
        {
          auto session = self->mSession.Lock();
          child = session->mChild;
        }
        if (child && child->CanSend()) {
          size_t sampleCount = audioData.Length();
          child->SendProcessAudioData(std::move(audioData));
          LOGV("Sent {} samples to HWInference", sampleCount);
        } else {
          LOGE("SpeechRecognitionChild not available, dropping {} samples",
               audioData.Length());
        }
      });
  sIPCCapability->Dispatch(sendAudio.forget());
}

void SpeechRecognitionBackend::StartSpeechRecognitionSession(
    const nsACString& aLanguage) {
  AssertOnIPCThread();

  RefPtr<hwinference::SpeechRecognitionChild> child;
  {
    auto session = mSession.Lock();
    if (session->mStopRequested) {
      // Shutdown() publishes mStopRequested and takes mChild away under this
      // same lock, so it cannot have seen this actor: closing it here is what
      // ends the session in the utility process, and not doing so would leave
      // the process-wide session slot taken for good.
      LOG("Session init skipped, teardown already requested");
      return;
    }
    RefPtr<mozilla::hwinference::HWInferenceManagerChild> hwInference =
        mozilla::hwinference::HWInferenceManagerChild::GetSingleton();
    session->mChild =
        hwInference ? hwInference->CreateSpeechRecognitionSession() : nullptr;
    child = session->mChild;
  }
  if (!child) {
    LOGE("Failed to create speech recognition session");
    HandleRecognitionError(nsCString("network"));
    return;
  }

  // The callbacks hold this instance with a strong ref, this instance holds the
  // callback with refs through mSession. This cycles is broken:
  // - from the IPC side in SpeechRecognitionChild::ActorDestroy
  // - from the main thread here in ::Shutdown()
  child->SetResultCallback(
      [self = RefPtr{this}](const nsCString& aTranscript, bool aIsFinal) {
        AssertOnIPCThread();
        LOG("Received recognition result: {} (final={})", aTranscript.get(),
            aIsFinal);
        self->HandleRecognitionResult(aTranscript, aIsFinal);
      });

  child->SetErrorCallback([self = RefPtr{this}](const nsCString& aError) {
    AssertOnIPCThread();
    LOGE("Recognition error: {}", aError.get());

    self->HandleRecognitionError(aError);
  });

  child->SetSpeechChangeCallback([self = RefPtr{this}](bool aSpeechDetected) {
    AssertOnIPCThread();
    LOG("Speech change: {}", aSpeechDetected ? "started" : "ended");

    self->DispatchToParentIfAlive(
        "SpeechRecognitionBackend::HandleSpeechChange",
        [speechDetected = aSpeechDetected](SpeechRecognition* aParent) {
          aParent->DispatchTrustedEvent(speechDetected ? u"speechstart"_ns
                                                       : u"speechend"_ns);
        });
  });

  child->SetDestroyedCallback(
      [self = RefPtr{this}](hwinference::SpeechRecognitionChild* aDestroyed) {
        AssertOnIPCThread();
        auto session = self->mSession.Lock();
        if (session->mChild == aDestroyed) {
          session->mChild = nullptr;
        }
      });

  child->SendInit(SPEECH_RECOGNITION_ENGINE_ID, aLanguage, mPhrases)
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
              // This needs to be a fallible dispatch because this promise can
              // resolve after e.g. `Stop()` has been called. In this case, it's
              // fine to fail to dispatch, it's what we want.
              nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
                  "SpeechRecognitionBackend::ProcessAudioOnBackgroundThread",
                  [self]() {
                    self->mResamplingCapability.AssertOnCurrentThread();
                    self->ProcessAudioChunk();
                  });
              self->mResamplingCapability.Dispatch(runnable.forget(),
                                                   NS_DISPATCH_FALLIBLE);
            }
          },
          [self = RefPtr{this}](ResponseRejectReason aReason) {
            LOGE("Init IPC call failed: {}", static_cast<int>(aReason));
            AssertOnIPCThread();
            self->HandleRecognitionError(nsCString("network"));
          });
}

void SpeechRecognitionBackend::HandleRecognitionResult(
    const nsACString& aTranscript, bool aIsFinal) {
  AssertOnIPCThread();
  LOG("HandleRecognitionResult: {} (final={})", nsCString(aTranscript).get(),
      aIsFinal);

  DispatchToParentIfAlive("SpeechRecognitionBackend::HandleRecognitionResult",
                          [transcript = nsCString(aTranscript),
                           aIsFinal](SpeechRecognition* aParent) {
                            aParent->HandleRecognitionResultFromBackend(
                                transcript, aIsFinal);
                          });
}

void SpeechRecognitionBackend::HandleRecognitionError(
    const nsACString& aError) {
  AssertOnIPCThread();
  LOGE("HandleRecognitionError: {}", nsCString(aError).get());

  DispatchToParentIfAlive(
      "SpeechRecognitionBackend::HandleRecognitionError",
      [error = nsCString(aError)](SpeechRecognition* aParent) {
        aParent->HandleRecognitionErrorFromBackend(error);
      });
}

void SpeechRecognitionBackend::NotifyTrackEnded() {
  DispatchToParentIfAlive("SpeechRecognitionBackend::NotifyTrackEnded",
                          [](SpeechRecognition* aParent) { aParent->Stop(); });
}

/* static */
nsCOMPtr<nsISerialEventTarget>
SpeechRecognitionBackend::GetOrCreateIPCThread() {
  AssertIsOnMainThread();

  if (!sIPCCapability) {
    // IPC actors are bound to the event target they were opened on, so the
    // target has to outlive them: a LazyIdleThread keeps a single one for the
    // process lifetime, and only releases its backing OS thread when idle.
    RefPtr<LazyIdleThread> thread =
        new LazyIdleThread(IPC_THREAD_IDLE_TIMEOUT_MS, "SpeechIPC");
    sIPCCapability = new EventTargetCapability<nsISerialEventTarget>(thread);
    LOG("Created shared IPC thread for speech recognition");
    ClearOnShutdown(&sIPCCapability);
  }

  return nsCOMPtr<nsISerialEventTarget>(sIPCCapability->GetEventTarget());
}

void SpeechRecognitionBackend::CloseIPCActorIfUnused() {
  AssertIsOnMainThread();
  // Close the HWInference connection once no session needs it, so the utility
  // process isn't kept alive by an idle connection. The serial event target is
  // kept (its backing OS thread is released on idle by the LazyIdleThread);
  // the next EnsureIPC() reopens the connection on that same target.
  if (!sIPCActorUsers && sIPCCapability) {
    nsCOMPtr<nsIRunnable> close = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::CloseHWInferenceChildIfAny",
        [] { CloseHWInferenceChildIfAny(); });
    sIPCCapability->Dispatch(close.forget());
  }
}

/* static */
void SpeechRecognitionBackend::AssertOnIPCThread() {
  sIPCCapability->AssertOnCurrentThread();
}

template <typename Func>
void SpeechRecognitionBackend::DispatchToParentIfAlive(const char* aName,
                                                       Func&& aFunc) {
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      aName,
      [self = RefPtr{this}, aFunc = std::forward<Func>(aFunc)]() mutable {
        AssertIsOnMainThread();
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

  using SendPromise = std::remove_pointer_t<
      decltype(aSendFunc(
                   static_cast<hwinference::SpeechRecognitionChild*>(nullptr),
                   aLanguages)
                   .get())>;
  using ResolveValueType = typename SendPromise::ResolveValueType;
  // Carries the outcome back to the main thread: aOnResult and aOnFailure
  // hold DOM promises, which must not be touched (nor released) elsewhere.
  using OperationPromise = MozPromise<ResolveValueType, nsresult, true>;
  using OperationPromisePrivate = typename OperationPromise::Private;

  RefPtr<OperationPromisePrivate> operation =
      new OperationPromisePrivate(__func__);

  RefPtr<IPCActorUserGuard> guard = EnsureIPC();
  nsCOMPtr<nsIRunnable> runSession = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::RunWithTransientSession",
      [operation, guard = std::move(guard), languages = std::move(aLanguages),
       aSendFunc]() mutable {
        AssertOnIPCThread();

        RefPtr<hwinference::HWInferenceManagerChild> manager =
            hwinference::HWInferenceManagerChild::GetSingleton();
        RefPtr<hwinference::SpeechRecognitionChild> child =
            manager ? manager->CreateSpeechRecognitionSession() : nullptr;
        if (!child) {
          operation->Reject(NS_ERROR_FAILURE, __func__);
          return;
        }
        // The connection is held open for exactly as long as this transient
        // session exists.
        child->SetIPCActorUserGuard(std::move(guard));

        aSendFunc(child, languages)
            ->Then(
                GetCurrentSerialEventTarget(), __func__,
                [operation,
                 child](typename SendPromise::ResolveOrRejectValue&& aValue) {
                  AssertOnIPCThread();
                  if (child->CanSend()) {
                    hwinference::SpeechRecognitionChild::Send__delete__(child);
                  }
                  if (aValue.IsReject()) {
                    operation->Reject(NS_ERROR_FAILURE, __func__);
                    return;
                  }
                  operation->Resolve(std::move(aValue.ResolveValue()),
                                     __func__);
                });
      });
  sIPCCapability->Dispatch(runSession.forget());

  operation->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [aOnResult](ResolveValueType aResult) mutable {
        aOnResult(std::move(aResult));
      },
      [aOnFailure](nsresult aError) { aOnFailure(); });
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Available(
    nsIGlobalObject* aGlobal, const nsTArray<nsCString>& aLanguages) {
  AssertIsOnMainThread();

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  nsTArray<nsCString> languages = aLanguages.Clone();
  if (languages.IsEmpty()) {
    languages.AppendElement("en-US"_ns);
  }

  LOG("SpeechRecognitionBackend::Available - Starting availability check for "
      "{} languages",
      languages.Length());

  if (MOZ_LOG_TEST(gSpeechRecognitionBackendLog, LogLevel::Debug)) {
    for (const auto& lang : languages) {
      LOG("SpeechRecognitionBackend::Available - Language requested: {}",
          lang.get());
    }
  }

  // https://webaudio.github.io/web-speech-api/#availability-algorithm
  // step 5.2.2: "available" if installed, "downloadable" if supported by
  // the user agent but not yet installed, "unavailable" if not supported.
  // IsModelAvailable alone can't tell "installed" from "not installed but
  // fetchable" apart (ModelHub.isModelAvailable returns true for both), so
  // check IsModelInstalled first for "available"; IsModelAvailable then
  // distinguishes "downloadable" from "unavailable".
  RefPtr<GenericPromise> installed = IsModelInstalledNative(languages.Clone());
  installed->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [promise, languages = std::move(languages)](
          const GenericPromise::ResolveOrRejectValue& aValue) mutable {
        AssertIsOnMainThread();
        if (aValue.IsResolve() && aValue.ResolveValue()) {
          LOG("SpeechRecognitionBackend::Available - model installed");
          promise->MaybeResolve(AvailabilityStatus::Available);
          return;
        }
        RunWithTransientSession(
            std::move(languages),
            [](hwinference::SpeechRecognitionChild* aChild,
               nsTArray<nsCString>& aLangs) {
              return aChild->SendIsModelAvailable(aLangs);
            },
            [promise](bool aAvailable) {
              LOG("SpeechRecognitionBackend::Available - IsModelAvailable: {}",
                  aAvailable ? "true" : "false");
              promise->MaybeResolve(aAvailable
                                        ? AvailabilityStatus::Downloadable
                                        : AvailabilityStatus::Unavailable);
            },
            [promise]() {
              promise->MaybeResolve(AvailabilityStatus::Unavailable);
            });
      });

  return promise.forget();
}

/* static */
RefPtr<GenericPromise> SpeechRecognitionBackend::IsModelInstalledNative(
    nsTArray<nsCString>&& aLanguages) {
  AssertIsOnMainThread();

  LOG("SpeechRecognitionBackend::IsModelInstalledNative - Starting installed "
      "check for {} languages",
      aLanguages.Length());

  RefPtr<GenericPromise::Private> resultPromise =
      new GenericPromise::Private(__func__);

  RunWithTransientSession(
      std::move(aLanguages),
      [](hwinference::SpeechRecognitionChild* aChild,
         nsTArray<nsCString>& aLangs) {
        return aChild->SendIsModelInstalled(aLangs);
      },
      [resultPromise](bool aInstalled) {
        LOG("SpeechRecognitionBackend::IsModelInstalledNative - Received "
            "response: {}",
            aInstalled ? "true" : "false");
        resultPromise->Resolve(aInstalled, __func__);
      },
      [resultPromise]() { resultPromise->Resolve(false, __func__); });

  return resultPromise;
}

/* static */
void SpeechRecognitionBackend::EnsureConnectedOnIPCThread() {
  AssertOnIPCThread();

  RefPtr<mozilla::hwinference::HWInferenceManagerChild> child =
      mozilla::hwinference::HWInferenceManagerChild::GetSingleton();
  if (child && child->CanSend()) {
    return;
  }

  // Binding the child endpoint here makes the connection immediately usable;
  // if the utility process cannot be launched, the parent endpoint is dropped
  // and the actor is simply destroyed asynchronously. Messages sent in the
  // meantime are queued by the channel.
  mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerParent> parentEp;
  mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerChild> childEp;
  MOZ_ALWAYS_SUCCEEDS(
      hwinference::PHWInferenceManager::CreateEndpoints(&parentEp, &childEp));
  mozilla::hwinference::HWInferenceManagerChild::OpenForProcess(
      std::move(childEp));

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::RequestHWInferenceConnection",
      [parentEp = std::move(parentEp)]() mutable {
        // Null in the parent process, which never connects this way.
        if (ContentChild* contentChild = ContentChild::GetSingleton()) {
          contentChild->SendRequestHWInferenceConnection(std::move(parentEp));
        }
      }));
}

already_AddRefed<IPCActorUserGuard> SpeechRecognitionBackend::EnsureIPC() {
  AssertIsOnMainThread();

  // Acquired before dispatching so that nothing can close the connection
  // between the setup below and the caller's use of it.
  RefPtr<IPCActorUserGuard> ipcActorUserGuard = new IPCActorUserGuard();

  nsCOMPtr<nsISerialEventTarget> ipcThread = GetOrCreateIPCThread();
  ipcThread->Dispatch(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::EnsureConnectedOnIPCThread",
      [] { EnsureConnectedOnIPCThread(); }));

  return ipcActorUserGuard.forget();
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Install(
    nsIGlobalObject* aGlobal, const nsTArray<nsCString>& aLanguages,
    uint64_t aInnerWindowId) {
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

  nsTArray<nsCString> languages = aLanguages.Clone();

  LOG("SpeechRecognitionBackend::Install - Starting install for {} languages",
      languages.Length());

  RunWithTransientSession(
      std::move(languages),
      [aInnerWindowId](hwinference::SpeechRecognitionChild* aChild,
                       nsTArray<nsCString>& aLangs) {
        return aChild->SendInstallModels(std::move(aLangs), aInnerWindowId);
      },
      [promise](bool aSuccess) {
        LOG("SpeechRecognitionBackend::Install - Install completed: {}",
            aSuccess ? "success" : "failed");
        promise->MaybeResolve(aSuccess);
      },
      [promise]() { promise->MaybeResolve(false); });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
