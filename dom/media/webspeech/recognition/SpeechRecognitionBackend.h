/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SpeechRecognitionBackend_h
#define mozilla_dom_SpeechRecognitionBackend_h

#include "AudioSegment.h"
#include "mozilla/EventTargetCapability.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ThreadSafeWeakPtr.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/WeakPtr.h"
#include "nsITargetShutdownTask.h"
#include "nsIThread.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::hwinference {
class HWInferenceManagerChild;
}  // namespace mozilla::hwinference

namespace mozilla {
class AudibilityMonitor;
class SpeechRecognitionChild;
namespace dom {
class AudioStreamTrack;
class SpeechRecognition;
class SpeechTrackListener;
}  // namespace dom
}  // namespace mozilla

namespace mozilla::dom {

class Promise;

class IPCThreadUserCounter {
 public:
  void Increment() MOZ_REQUIRES(sMainThreadCapability);
  void Decrement() MOZ_REQUIRES(sMainThreadCapability);
  bool IsZero() const MOZ_REQUIRES(sMainThreadCapability);

 private:
  int mCount = 0;
};

class SpeechRecognitionBackend;

class TransientSpeechRecognitionSession {
 public:
  TransientSpeechRecognitionSession();
  ~TransientSpeechRecognitionSession();

  SpeechRecognitionChild* get() const { return mChild; }
  SpeechRecognitionChild* operator->() const { return mChild; }

 private:
  RefPtr<SpeechRecognitionChild> mChild;
};

class SpeechRecognitionBackend
    : public SupportsThreadSafeWeakPtr<SpeechRecognitionBackend> {
  friend class IPCThreadUserCounter;
  friend class TransientSpeechRecognitionSession;

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
      nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages);
  static already_AddRefed<Promise> Install(
      nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages);

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

  // Holds the shared IPC thread alive for as long as it's held, releasing on
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
  // possible to use the shared IPC thread without also holding a reference
  // that keeps it alive for as long as it's needed.
  class IPCThreadUserGuard final {
   public:
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(IPCThreadUserGuard)

   private:
    friend class SpeechRecognitionBackend;
    IPCThreadUserGuard() { SpeechRecognitionBackend::AcquireIPCThreadUser(); }
    ~IPCThreadUserGuard();
  };

  struct EnsureIPCResult {
    // Resolves(true) once the connection is ready, or rejects on failure.
    RefPtr<GenericPromise> mReady;
    // Null iff mReady is a pre-rejected promise (the shared thread couldn't
    // be created at all). Callers that need the connection beyond mReady
    // settling (i.e. everyone but a simple one-shot check) must keep this
    // alive for as long as they do.
    RefPtr<IPCThreadUserGuard> mGuard;
  };
  static EnsureIPCResult EnsureIPC() MOZ_REQUIRES(sMainThreadCapability);

  static nsCOMPtr<nsIThread> GetOrCreateIPCThread();

  static void AssertOnIPCThread() MOZ_ASSERT_CAPABILITY(sIPCCapability);
  static void StopIPCThreadIfPossible();

  // Closes sHWInferenceChild if open. Called on the IPC thread, both from a
  // live idle-close (StopIPCThreadIfPossible) and from the thread's own
  // shutdown (ShutdownTask).
  static void CloseHWInferenceChildIfAny();

  // Closes sHWInferenceChild as part of the IPC thread's own shutdown
  // sequence (see nsIEventTarget::RegisterShutdownTask), so closing the actor
  // never races a separately-dispatched runnable against the thread's queue
  // closing.
  class ShutdownTask final : public nsITargetShutdownTask {
   public:
    NS_DECL_THREADSAFE_ISUPPORTS

    void TargetShutdown() override;

   private:
    ~ShutdownTask() = default;
  };
  static void AcquireIPCThreadUser();
  static void ReleaseIPCThreadUser();

  template <typename Func>
  static void OnIPCThread(Func&& aFunc) MOZ_ASSERT_CAPABILITY(sIPCCapability);

  // Shared by Available(), IsModelInstalledNative(), GetModelDownloadSize(),
  // and Install(): waits for the shared IPC connection, opens a transient
  // session, and calls aSendFunc(session, languages) to make the actual
  // HWInference request. aOnResult runs on the main thread with the IPC
  // call's resolved value; aOnFailure runs on the main thread with no value
  // for any failure along the way (EnsureIPC, session creation, or the IPC
  // call itself rejecting).
  template <typename SendFunc, typename OnResult, typename OnFailure>
  static void RunWithTransientSession(nsTArray<nsCString>&& aLanguages,
                                      SendFunc aSendFunc, OnResult aOnResult,
                                      OnFailure aOnFailure)
      MOZ_REQUIRES(sMainThreadCapability);

  static StaticRefPtr<nsIThread> sIPCThread;

 public:
  static mozilla::EventTargetCapability<nsIThread>* sIPCCapability;

 private:
  static StaticRefPtr<mozilla::hwinference::HWInferenceManagerChild>
      sHWInferenceChild;

  static IPCThreadUserCounter sIPCThreadUsers;

  WeakPtr<SpeechRecognition> mParent;
  nsCString mLanguage;
  nsTArray<nsString> mPhrases;

  // Per-instance IPC channel for speech recognition sessions
  RefPtr<SpeechRecognitionChild> mSpeechRecognitionChild;
};

}  // namespace mozilla::dom

#endif
