/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SpeechRecognitionBackend_h
#define mozilla_dom_SpeechRecognitionBackend_h

#include "AudioSegment.h"
#include "mozilla/EventTargetCapability.h"
#include "mozilla/LazyIdleThread.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ThreadSafeWeakPtr.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/hwinference/PHWInferenceManagerChild.h"
#include "mozilla/ipc/Endpoint.h"
#include "nsIThread.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::hwinference {
class SpeechRecognitionChild;
}  // namespace mozilla::hwinference

namespace mozilla {
class AudibilityMonitor;
namespace dom {
class AudioStreamTrack;
class SpeechRecognition;
class SpeechTrackListener;
}  // namespace dom
}  // namespace mozilla

namespace mozilla::dom {

class Promise;

class SpeechRecognitionBackend;

class SpeechRecognitionBackend
    : public SupportsThreadSafeWeakPtr<SpeechRecognitionBackend> {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING_WITH_DELETE_ON_MAIN_THREAD(
      SpeechRecognitionBackend)

  SpeechRecognitionBackend(SpeechRecognition* aParent, uint32_t aGraphRate,
                           const nsString& aLanguage,
                           const nsTArray<nsString>& aPhrases)
      MOZ_REQUIRES(sMainThreadCapability);
  // Called when SpeechRecognition.start() is called from JS.
  // Starts the background thread and IPC session.
  // Creates background thread and establishes IPC connection.
  nsresult Start() MOZ_REQUIRES(sMainThreadCapability);
  // Called when SpeechRecognition.stop() is called from JS.
  // Stops the background thread and IPC session.
  // Gracefully shuts down background thread and closes IPC.
  void Stop() MOZ_REQUIRES(sMainThreadCapability);
  // Called when SpeechRecognition.abort() is called from js.
  // Aborts the recognition session.
  // Immediately terminates background thread and IPC.
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
  void DataCallback(TrackTime aTime, const AudioChunk& aChunk);
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

 private:
  virtual ~SpeechRecognitionBackend();

  // == Resampling thread
  void StartProcessingAudioOnBackgroundThread()
      MOZ_REQUIRES(mResamplingCapability);
  void ProcessAudioChunk() MOZ_REQUIRES(mResamplingCapability);
  void SendAudioDataViaIPC(nsTArray<float>&& aAudioData)
      MOZ_REQUIRES(mResamplingCapability);

  // == IPC thread
  void StartSpeechRecognitionSession(const nsCString& aLanguage)
      MOZ_REQUIRES(sIPCCapability);
  void StopSpeechRecognitionSession() MOZ_REQUIRES(sIPCCapability);
  void HandleRecognitionResult(const nsCString& aTranscript, bool aIsFinal,
                               float aConfidence, TimeStamp aEventTime)
      MOZ_REQUIRES(sIPCCapability);
  void HandleRecognitionError(const nsCString& aError)
      MOZ_REQUIRES(sIPCCapability);

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
  // The only way to obtain one is via EnsureIPC()'s return value, so it's not
  // possible to use the shared actor without also holding a reference that
  // keeps it open for as long as it's needed.
  class IPCActorUserGuard final {
   public:
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(IPCActorUserGuard)

   private:
    friend class SpeechRecognitionBackend;
    IPCActorUserGuard() { SpeechRecognitionBackend::AcquireIPCActorUser(); }
    ~IPCActorUserGuard();
  };

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

  WeakPtr<SpeechRecognition> mParent;
  nsCString mLanguage;
  nsTArray<nsString> mPhrases;

  mozilla::EventTargetCapability<nsIThread> mResamplingCapability;

  // Per-instance IPC channel for speech recognition sessions
  RefPtr<hwinference::SpeechRecognitionChild> mSpeechRecognitionChild;
};

}  // namespace mozilla::dom

#endif
