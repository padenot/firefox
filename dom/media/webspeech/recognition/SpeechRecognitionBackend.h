/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONBACKEND_H_
#define DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONBACKEND_H_

#include <atomic>

#include "AudioConverter.h"
#include "AudioSegment.h"
#include "MainThreadUtils.h"
#include "mozilla/EventTargetCapability.h"
#include "mozilla/RefPtr.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ThreadSafeWeakPtr.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/dom/Promise.h"
#include "nsISupports.h"
#include "nsTArray.h"

namespace mozilla::hwinference {
class HWInferenceManagerChild;
}  // namespace mozilla::hwinference

namespace mozilla {
class AudibilityMonitor;
class SpeechRecognitionChild;
namespace dom {
  class SpeechRecognition;
}
}

namespace mozilla::dom {

class SpeechRecognitionBackend
    : public nsISupports,
      public SupportsThreadSafeWeakPtr<SpeechRecognitionBackend> {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  // == Static methods (Main thread only)
  // Check if a speech recognition model is available for given languages.
  static already_AddRefed<Promise> Available(
      nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages)
      MOZ_REQUIRES(sMainThreadCapability);
  // Install speech recognition models for given languages.
  static already_AddRefed<Promise> Install(nsIGlobalObject* aGlobal,
                                           const nsTArray<nsString>& aLanguages)
      MOZ_REQUIRES(sMainThreadCapability);

  // == Main thread only
  // Biasing phrases are passed in the ctor for now, but see:
  // https://github.com/WebAudio/web-speech-api/issues/172
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

  // == Graph thread
  // Called by SpeechTrackListener on the graph's real-time thread
  // Uses lock-free SPSC queue to send data to background thread
  void DataCallback(TrackTime aTime, const AudioChunk& aChunk);

 protected:
  virtual ~SpeechRecognitionBackend();

 private:
  // == Resampling thread
  // Initialize IPC if needed and kicks off the processing loop.
  void StartProcessingAudioOnBackgroundThread()
      MOZ_REQUIRES(mResamplingCapability);
  // Process a chunk of audio data: resamples audio to 16kHz and sends to
  // HWInference process.
  void ProcessAudioChunk() MOZ_REQUIRES(mResamplingCapability);
  // Send audio data to HWInference process
  void SendAudioDataViaIPC(nsTArray<float>&& aAudioData)
      MOZ_REQUIRES(mResamplingCapability);

  // == IPC thread
  void StartSpeechRecognitionSession(const nsCString& aLanguage)
      MOZ_REQUIRES(sIPCCapability);
  void StopSpeechRecognitionSession() MOZ_REQUIRES(sIPCCapability);
  // Called via IPC with recognition results. This dispatches the result to the
  // main thread, utltimately calling the SpeechRecognition DOM object
  void HandleRecognitionResult(const nsCString& aTranscript, bool aIsFinal)
      MOZ_REQUIRES(sIPCCapability);
  // Similarly, propagate error from the backend (received via IPC) to the
  // SpeechRecognition DOM object on the main thread.
  // All errors are currently considered fatal and will terminate recognition.
  void HandleRecognitionError(const nsCString& aError)
      MOZ_REQUIRES(sIPCCapability);

  // Utility method to ensure IPC connection is initialized.
  // Returns a promise that resolves when the connection is ready.
  static RefPtr<GenericPromise> EnsureIPC() MOZ_REQUIRES(sMainThreadCapability);

  // Get or create the shared IPC thread for all speech recognition IPC
  // operations. This ensures all IPC happens on a single thread to avoid
  // assertions.
  static nsCOMPtr<nsIThread> GetOrCreateIPCThread();

  // Assert that the current thread is the shared (static) IPC thread
  static void AssertOnIPCThread() MOZ_ASSERT_CAPABILITY(sIPCCapability);
  // Attempt to stop the shared IPC thread if it's no longer needed, e.g. no
  // recognition active, all static calls finished.
  static void StopIPCThreadIfPossible();
  // Assert that the current thread is the resampling thread (dedicated instance
  // thread).
  void AssertOnResamplingThread() MOZ_ASSERT_CAPABILITY(mResamplingCapability);

  // Helper to dispatch a function to the IPC thread with automatic assertion
  template <typename Func>
  static void OnIPCThread(Func&& aFunc) MOZ_ASSERT_CAPABILITY(sIPCCapability);

  // Shared background thread for all IPC operations
  static StaticRefPtr<nsIThread> sIPCThread;

  // Thread safety capability for the shared IPC thread
  static mozilla::EventTargetCapability<nsIThread>* sIPCCapability;

  // Shared IPC channel to HWInference process
  static StaticRefPtr<mozilla::hwinference::HWInferenceManagerChild> sHWInferenceChild;

  // Number of users of the IPC thread. Incremented when available/install/start
  // is called, decremented when available/install are settled, or on abort.
  static int sIPCThreadUsers;

  // Parent SpeechRecognition object -- always valid because the lifetime of
  // mParent exceeds the lifetime of this object.
  RefPtr<SpeechRecognition> mParent;

  // Language for recognition
  // Set on main thread, read on background thread during IPC setup
  const nsCString mLanguage;
  // Phrases for recognition biasing.
  // Written on main thread, read on background thread.
  const nsTArray<nsString> mPhrases;
  // Lock-free SPSC queue for transferring audio from graph to background thread
  // Producer: graph thread (DataCallback), Consumer: mThread
  // Lifetime: Created in Start(), destroyed in Stop()/Abort()
  UniquePtr<SPSCQueue<float>> mRingBuffer;
  // Consumes audio from mRingBuffer, resamples, and sends to the HWInference
  // process via the IPC thread
  // Lifetime: Created in Start(), shutdown in Stop()/Abort()
  nsCOMPtr<nsIThread> mResamplingThread;
  mozilla::EventTargetCapability<nsIThread> mResamplingCapability;
  // Pre-allocated buffer for mono downmixing.
  // Graph thread only.
  // Lifetime: Allocated in Start(), deallocated in Stop()/Abort()
  nsTArray<AudioDataValue> mMonoBuffer;
  // Thread control flag for resampling thread thread processing loop
  std::atomic<bool> mResamplingThreadRunning{false};
  // Graph sample rate
  // Set once in Initialize(), read by background thread for resampling
  uint32_t mGraphRate = 0;
  // Sound detection for soundstart/soundend events
  // Updated by resampling thread, events dispatched to main thread
  bool mCurrentlyAudible = false;
  // Track if we've dispatched audiostart event
  bool mAudioStartDispatched = false;
  // Audibility monitor for detecting sound (500ms silence duration)
  UniquePtr<mozilla::AudibilityMonitor> mAudibilityMonitor;
  // Per-instance IPC channel for speech recognition sessions
  RefPtr<mozilla::SpeechRecognitionChild> mSpeechRecognitionChild;
  // Converts from graph rate to ASR model rate (16kHz)
  UniquePtr<AudioConverter> mAudioConverter;
};

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONBACKEND_H_
