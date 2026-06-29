/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SpeechRecognitionBackend_h
#define mozilla_dom_SpeechRecognitionBackend_h

#include <atomic>

#include "AudioConverter.h"
#include "AudioSegment.h"
#include "MainThreadUtils.h"
#include "SpeechTrackListener.h"
#include "mozilla/EventTargetCapability.h"
#include "mozilla/RefPtr.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ThreadSafeWeakPtr.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/Promise.h"
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
class Promise;
}  // namespace dom
}  // namespace mozilla

namespace mozilla::dom {

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
                               float aConfidence) MOZ_REQUIRES(sIPCCapability);
  void HandleRecognitionError(const nsCString& aError)
      MOZ_REQUIRES(sIPCCapability);

  static RefPtr<GenericPromise> EnsureIPC() MOZ_REQUIRES(sMainThreadCapability);

  static nsCOMPtr<nsIThread> GetOrCreateIPCThread();

  static void AssertOnIPCThread() MOZ_ASSERT_CAPABILITY(sIPCCapability);
  static void StopIPCThreadIfPossible();

  // Mark a transient operation (Available/Install) as using the shared IPC
  // thread. Acquire when the operation starts and Release when its promise
  // settles, both on the main thread, so the thread stays alive for the whole
  // operation and is torn down once no operation needs it.
 public:
  static void AcquireIPCThreadUser();
  static void ReleaseIPCThreadUser();

 private:
  void AssertOnResamplingThread() MOZ_ASSERT_CAPABILITY(mResamplingCapability);

  template <typename Func>
  static void OnIPCThread(Func&& aFunc) MOZ_ASSERT_CAPABILITY(sIPCCapability);

  template <typename Func>
  void DispatchToParentIfAlive(const char* aName, Func&& aFunc);

  static StaticRefPtr<nsIThread> sIPCThread;

 public:
  static mozilla::EventTargetCapability<nsIThread>* sIPCCapability;

 private:
  static StaticRefPtr<mozilla::hwinference::HWInferenceManagerChild>
      sHWInferenceChild;

  static IPCThreadUserCounter sIPCThreadUsers;

  WeakPtr<SpeechRecognition> mParent;

  RefPtr<AudioStreamTrack> mTrack;
  RefPtr<SpeechTrackListener> mTrackListener;

  const nsCString mLanguage;
  const nsTArray<nsString> mPhrases;
  UniquePtr<SPSCQueue<float>> mRingBuffer;
  nsCOMPtr<nsIThread> mResamplingThread;
  mozilla::EventTargetCapability<nsIThread> mResamplingCapability;
  nsTArray<AudioDataValue> mMonoBuffer;
  std::atomic<bool> mResamplingThreadRunning{false};
  uint32_t mGraphRate = 0;
  bool mCurrentlyAudible = false;
  bool mAudioStartDispatched = false;
  UniquePtr<mozilla::AudibilityMonitor> mAudibilityMonitor;
  RefPtr<SpeechRecognitionChild> mSpeechRecognitionChild;
  UniquePtr<AudioConverter> mAudioConverter;
};

}  // namespace mozilla::dom

#endif
