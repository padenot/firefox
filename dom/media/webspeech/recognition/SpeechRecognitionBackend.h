/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SpeechRecognitionBackend_h
#define mozilla_dom_SpeechRecognitionBackend_h

#include "AudioSegment.h"
#include "MainThreadUtils.h"
#include "mozilla/DataMutex.h"
#include "mozilla/EventTargetCapability.h"
#include "mozilla/LazyIdleThread.h"
#include "mozilla/RefPtr.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/hwinference/PHWInferenceManagerChild.h"
#include "mozilla/ipc/Endpoint.h"
#include "nsIThread.h"
#include "nsITimer.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::hwinference {
class SpeechRecognitionChild;
}  // namespace mozilla::hwinference

namespace mozilla {
class AudibilityMonitor;
class AudioConverter;
class MediaTrackGraph;
namespace dom {
class AudioStreamTrack;
class SpeechRecognition;
class SpeechTrackListener;
}  // namespace dom
}  // namespace mozilla

namespace mozilla::dom {

class Promise;

class SpeechRecognitionBackend;

// Holds the shared IPC actor open for as long as it's held, releasing on
// destruction - tied to the guard's own lifetime rather than to a promise
// settling. Gecko silently drops a promise's reaction jobs, including a
// PromiseNativeHandler added via AppendNativeHandler, once the promise's
// global has died (e.g. the calling iframe was detached before the async
// IPC round trip completed); tying the hold to this refcounted guard
// instead avoids leaking it - and everything it transitively keeps alive,
// such as the shared HWInferenceManagerChild connection - forever whenever
// a caller's frame goes away mid-flight.
//
// Obtained from EnsureIPC() (which also establishes the connection) or from
// AcquireProcessKeepAlive() (which only counts), so the shared actor cannot
// be used without something holding it open for as long as it is needed.
class IPCActorUserGuard final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(IPCActorUserGuard)

 private:
  friend class SpeechRecognitionBackend;
  IPCActorUserGuard();
  ~IPCActorUserGuard();
};

class SpeechRecognitionBackend {
  friend class IPCActorUserGuard;

 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING_WITH_DELETE_ON_MAIN_THREAD(
      SpeechRecognitionBackend)

  // Creates a backend, along with the resampling thread it processes audio on.
  // Returns nullptr if that thread cannot be created.
  static already_AddRefed<SpeechRecognitionBackend> Create(
      SpeechRecognition* aParent, uint32_t aGraphRate,
      const nsString& aLanguage, const nsTArray<nsString>& aPhrases)
      MOZ_REQUIRES(sMainThreadCapability);

  // Called when SpeechRecognition.start() is called from JS. Establishes the
  // IPC connection; the resampling loop only starts once the engine has
  // confirmed session init.
  void Start() MOZ_REQUIRES(sMainThreadCapability);
  // Called when SpeechRecognition.stop() is called from JS. Shuts down the
  // background thread and IPC session, waiting for the engine's end-of-stream
  // flush before reporting the session finished.
  void Stop() MOZ_REQUIRES(sMainThreadCapability);
  // Called when SpeechRecognition.abort() is called from JS, and from
  // SpeechRecognition's own teardown. Immediately terminates the resampling
  // thread and IPC, discarding any result the engine had yet to flush.
  void Abort() MOZ_REQUIRES(sMainThreadCapability);

  // Attach to an audio track to start receiving audio data.
  // Creates a SpeechTrackListener and attaches it to the track.
  void AttachToTrack(AudioStreamTrack* aTrack)
      MOZ_REQUIRES(sMainThreadCapability);
  // Detach from the current audio track.
  void DetachFromTrack() MOZ_REQUIRES(sMainThreadCapability);

  // == Graph thread
  // Called by SpeechTrackListener on the graph's real-time thread
  // Uses lock-free SPSC queue to send data to background thread
  void DataCallback(MediaTrackGraph* aGraph, TrackTime aTime,
                    const AudioChunk& aChunk);
  // Called by SpeechTrackListener when the track ends
  void NotifyTrackEnded();

  static already_AddRefed<Promise> Available(
      nsIGlobalObject* aGlobal, const nsTArray<nsCString>& aLanguages);
  static already_AddRefed<Promise> Install(
      nsIGlobalObject* aGlobal, const nsTArray<nsCString>& aLanguages);
  // Resolves(true) iff the model is already downloaded to the local cache,
  // so install() can skip its permission prompt when there is nothing to
  // download. Resolves(false) (never rejects) otherwise.
  static RefPtr<GenericPromise> IsModelInstalledNative(
      nsTArray<nsCString>&& aLanguages);

  // Keeps the HWInference process alive without establishing a connection,
  // for a live SpeechRecognition object that has not called start() yet.
  static already_AddRefed<IPCActorUserGuard> AcquireProcessKeepAlive()
      MOZ_REQUIRES(sMainThreadCapability);

 private:
  SpeechRecognitionBackend(SpeechRecognition* aParent,
                           nsIThread* aResamplingThread, uint32_t aGraphRate,
                           const nsString& aLanguage,
                           const nsTArray<nsString>& aPhrases)
      MOZ_REQUIRES(sMainThreadCapability);
  virtual ~SpeechRecognitionBackend();

  // Shared body of Stop() and Abort(). aWaitForFlush defers
  // NotifySessionFinished() until the engine has flushed and answered
  // PSpeechRecognition::Stop.
  void Shutdown(bool aWaitForFlush) MOZ_REQUIRES(sMainThreadCapability);
  // Tells the SpeechRecognition the session is over and whether the engine
  // finalized anything, so it can fire nomatch before end. Callable from the
  // main and IPC threads.
  void NotifySessionFinished(bool aProducedResult);

  // == Resampling thread
  void ProcessAudioChunk() MOZ_REQUIRES(mResamplingCapability);
  void SendAudioDataViaIPC(nsTArray<float>&& aAudioData)
      MOZ_REQUIRES(mResamplingCapability);

  // == IPC thread
  void StartSpeechRecognitionSession(const nsACString& aLanguage)
      MOZ_REQUIRES(sIPCCapability);
  void HandleRecognitionResult(const nsACString& aTranscript, bool aIsFinal)
      MOZ_REQUIRES(sIPCCapability);
  void HandleRecognitionError(const nsACString& aError)
      MOZ_REQUIRES(sIPCCapability);

  // Returns a guard holding the shared connection open, and kicks off the
  // connection setup on the IPC thread. Anything dispatched to the IPC thread
  // afterwards can use HWInferenceManagerChild::GetSingleton() right away.
  static already_AddRefed<IPCActorUserGuard> EnsureIPC()
      MOZ_REQUIRES(sMainThreadCapability);
  // Binds this process' side of a new HWInference connection, and asks the
  // parent process for the other side. No waiting: the actor is simply
  // destroyed asynchronously if the connection cannot be completed.
  static void EnsureConnectedOnIPCThread();

  // Returns the shared IPC thread's serial event target, creating the thread
  // on first use. The target is stable for the process lifetime; its backing
  // OS thread is released when idle (see GetOrCreateIPCThread's body).
  static nsCOMPtr<nsISerialEventTarget> GetOrCreateIPCThread();

  static void AssertOnIPCThread() MOZ_ASSERT_CAPABILITY(sIPCCapability);
  static void CloseIPCActorIfUnused();

  // Closes HWInferenceManagerChild::GetSingleton() if open. Called on the IPC
  // thread from the idle-close in CloseIPCActorIfUnused.
  static void CloseHWInferenceChildIfAny();

  static void AcquireIPCActorUser();
  static void ReleaseIPCActorUser();
  static void CancelIdleCloseTimer() MOZ_REQUIRES(sMainThreadCapability);

  // Shared by Available(), IsModelInstalledNative(), and Install(): opens a
  // transient session on the shared IPC connection, and calls
  // aSendFunc(session, languages) to make the actual HWInference request.
  // aOnResult runs on the main thread with the IPC call's resolved value;
  // aOnFailure runs on the main thread with no value for any failure along
  // the way (session creation or the IPC call itself rejecting).
  template <typename SendFunc, typename OnResult, typename OnFailure>
  static void RunWithTransientSession(nsTArray<nsCString>&& aLanguages,
                                      SendFunc aSendFunc, OnResult aOnResult,
                                      OnFailure aOnFailure)
      MOZ_REQUIRES(sMainThreadCapability);

  // Runs aFunc(parent) on the main thread, named aName, if the parent
  // SpeechRecognition is still alive and still owns this backend by then.
  // Callable from any thread.
  template <typename Func>
  void DispatchToParentIfAlive(const char* aName, Func&& aFunc);

 public:
  static StaticAutoPtr<mozilla::EventTargetCapability<nsISerialEventTarget>>
      sIPCCapability;

 private:
  // Number of live IPCActorUserGuards, i.e. of things that need the shared
  // HWInference connection: a live SpeechRecognition object, a session, or a
  // static call in flight.
  static int32_t sIPCActorUsers MOZ_GUARDED_BY(sMainThreadCapability);
  // Armed when sIPCActorUsers hits zero, cancelled by the next acquisition, so
  // the connection survives a brief gap between users. See
  // media.webspeech.recognition.idle_shutdown_grace_ms.
  static StaticRefPtr<nsITimer> sIdleCloseTimer
      MOZ_GUARDED_BY(sMainThreadCapability);

  // Upgraded to a RefPtr on the main thread only; see DispatchToParentIfAlive.
  WeakPtr<SpeechRecognition> mParent MOZ_GUARDED_BY(sMainThreadCapability);

  RefPtr<AudioStreamTrack> mTrack MOZ_GUARDED_BY(sMainThreadCapability);
  RefPtr<SpeechTrackListener> mTrackListener
      MOZ_GUARDED_BY(sMainThreadCapability);

  // Read on the IPC thread, when initializing a session.
  const nsCString mLanguage;
  const nsTArray<nsString> mPhrases;
  // Written by the graph thread (DataCallback), read by the resampling thread
  // (ProcessAudioChunk). There is no capability for the graph thread, hence no
  // annotation here.
  const UniquePtr<SPSCQueue<float>> mRingBuffer;
  // Created by Create() and shut down by Shutdown(), both on the main thread,
  // and touched by nobody else - the resampling thread reaches itself through
  // mResamplingCapability. Sole owner of the thread's lifetime.
  nsCOMPtr<nsIThread> mResamplingThread MOZ_GUARDED_BY(sMainThreadCapability);
  // Guards the members only the resampling thread may touch.
  const mozilla::EventTargetCapability<nsIThread> mResamplingCapability;
  // Graph-thread downmixing scratch buffer, freed with the backend after
  // DetachFromTrack() has stopped the callbacks.
  nsTArray<AudioDataValue> mMonoBuffer;
  const uint32_t mGraphRate;
  // Whether Shutdown() has already run, so it runs exactly once.
  bool mStopped MOZ_GUARDED_BY(sMainThreadCapability) = false;
  // Whether a soundstart was fired without its soundend. The main thread owns
  // the soundstart/soundend pair, the resampling thread only reports the
  // transitions it sees, so that teardown - which happens here - is what
  // decides how the session ends.
  bool mCurrentlyAudible MOZ_GUARDED_BY(sMainThreadCapability) = false;
  // Last audibility the monitor reported, to detect transitions.
  bool mAudible MOZ_GUARDED_BY(mResamplingCapability) = false;
  bool mAudioStartDispatched MOZ_GUARDED_BY(mResamplingCapability) = false;
  // Set by a task Shutdown() dispatches to the resampling thread, and never
  // cleared, so the loop stops and cannot be restarted by a session init that
  // completes after teardown. This is the only thing that stops the loop: the
  // audio path never takes a lock or reads an atomic to decide whether to
  // continue.
  bool mAudioProcessingStopped MOZ_GUARDED_BY(mResamplingCapability) = false;
  // Created by Start() on main, before the resampling thread that uses it.
  UniquePtr<mozilla::AudibilityMonitor> mAudibilityMonitor;
  // Held for the lifetime of an active session (Start() to Stop()/Abort()),
  // set from EnsureIPC()'s result. See IPCActorUserGuard.
  RefPtr<IPCActorUserGuard> mIPCActorUserGuard
      MOZ_GUARDED_BY(sMainThreadCapability);

  // The current session's actor, and whether teardown has been requested.
  // mStopRequested is how Shutdown() on main tells the IPC thread, which can be
  // in the middle of opening a session, that there is no longer a session to
  // open one for. Publishing it and taking mChild away happen together under
  // this lock, so exactly one of the two sides ends up owning the actor: either
  // the IPC thread stores it and Shutdown() stops it, or the IPC thread sees
  // the flag and closes it itself. See mState in
  // dom/workers/remoteworkers/RemoteWorkerChild.h for the same pattern.
  struct Session {
    RefPtr<hwinference::SpeechRecognitionChild> mChild;
    bool mStopRequested = false;
  };
  DataMutex<Session> mSession{"SpeechRecognitionBackend::mSession"};

  UniquePtr<AudioConverter> mAudioConverter
      MOZ_GUARDED_BY(mResamplingCapability);
};

}  // namespace mozilla::dom

#endif
