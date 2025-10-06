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
#include "mozilla/SPSCQueue.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/Promise.h"
#include "nsISupports.h"
#include "nsTArray.h"

namespace mozilla::ipc {
class HWInferenceManagerChild;
class SpeechRecognitionChild;
}  // namespace mozilla::ipc

namespace mozilla::dom {

class SpeechRecognition;

class SpeechRecognitionBackend : public nsISupports, public SupportsWeakPtr {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  // == Static methods (Main thread only)
  // Check if a speech recognition model is available for given languages
  static already_AddRefed<Promise> Available(
      nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages);
  // Install speech recognition models for given languages
  static already_AddRefed<Promise> Install(
      nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages);

  // == Main thread only
  // Biasing phrases are passed in the ctor for now, but see:
  // https://github.com/WebAudio/web-speech-api/issues/172
  SpeechRecognitionBackend(SpeechRecognition* aParent, uint32_t aGraphRate,
                           const nsString& aLanguages,
                           const nsTArray<nsString>& aPhrases);
  // Called when SpeechRecognition.start() is called from JS
  // Starts the background thread and IPC session
  // Creates background thread and establishes IPC connection
  nsresult Start(uint64_t aSessionId);
  // Called when SpeechRecognition.stop() is called from JS
  // Stops the background thread and IPC session
  // Gracefully shuts down background thread and closes IPC
  void Stop();
  // Called when SpeechRecognition.abort() is called from js
  // Aborts the recognition session
  // Immediately terminates background thread and IPC
  void Abort();

  // == Graph thread
  // Called by SpeechTrackListener on the graph's real-time thread
  // Uses lock-free SPSC queue to send data to background thread
  void DataCallback(TrackTime aTime, const AudioChunk& aChunk);

 protected:
  // Destructor - ensures background thread is stopped
  virtual ~SpeechRecognitionBackend();

 private:
  // == Instance background thread
  // Initialize IPC if needed and kicks of the processing loop.
  void StartProcessingAudioOnBackgroundThread();
  // Process a chunk of audio data
  // Resamples audio to 16kHz and sends to HWInference process
  void ProcessAudioChunk();

  // Send audio data to HWInference process
  void SendAudioDataViaIPC(uint64_t aSessionId, nsTArray<float>&& aAudioData,
                           uint32_t aSampleRate);

  void StartSpeechRecognitionSession(uint64_t aSessionId,
                                     const nsCString& aLanguage);
  // Stop the current recognition session
  void StopSpeechRecognitionSession(uint64_t aSessionId);
  // Called via IPC with recognition results. This dispatches the result to the
  // main thread, calling the SpeechRecognition object
  void HandleRecognitionResult(const nsCString& aTranscript, bool aIsFinal);
  // Similarly, propagate error from the backend (received via IPC) to the
  // SpeechRecognition object on the main thread
  void HandleRecognitionError(const nsCString& aError);

  // Utility method to ensure IPC connection is initialized
  // Returns a promise that resolves when the connection is ready
  // Must be called on the main thread
  static RefPtr<mozilla::GenericPromise> EnsureIPC();

  // Get or create the shared IPC thread for all speech recognition IPC
  // operations This ensures all IPC happens on a single thread to avoid
  // assertions
  static RefPtr<nsIThread> GetOrCreateIPCThread();

  // Assert that the current thread is the shared IPC thread
  static void AssertOnIPCThread();
  // Assert that the current thread is the resampling thread
  void AssertOnResamplingThread();

  // Helper to dispatch a function to the IPC thread with automatic assertion
  template <typename Func>
  static void OnIPCThread(Func&& aFunc);

  // TODO Make this not static. A backend can be created for Available and Install
  // Shared background thread for all IPC operations
  // This thread is never shutdown to ensure consistent IPC access
  static StaticRefPtr<nsIThread> sIPCThread;

  // Shared IPC channel to HWInference process
  static StaticRefPtr<mozilla::ipc::HWInferenceManagerChild> sHWInferenceChild;

  // Parent SpeechRecognition object -- always valid because the lifetime of
  // mParent exceeds the lifetime of this object.
  RefPtr<SpeechRecognition> mParent;

  // [Main thread write, Background thread read] Language for recognition
  // Set on main thread, read on background thread during IPC setup
  const nsCString mLanguage;
  // Written on main thread, read on background thread.
  // Phrases for recognition biasing.
  const nsTArray<nsString> mPhrases;
  // [Graph thread write, Resampling thread read] Audio data queue
  // Lock-free SPSC queue for transferring audio from graph to background thread
  // Producer: graph thread (DataCallback), Consumer: mThread
  // Lifetime: Created in Start(), destroyed in Stop()/Abort()
  UniquePtr<SPSCQueue<float>> mRingBuffer;

  // [Main thread create/destroy, runs independently] Background processing
  // thread Consumes audio from mRingBuffer, resamples, and sends to HWInference
  // Lifetime: Created in Start(), shutdown in Stop()/Abort()
  nsCOMPtr<nsIThread> mResamplingThread;

  // [Graph thread only] Pre-allocated buffer for real-time audio downmixing
  // Used in DataCallback to avoid allocations on real-time thread
  // Lifetime: Allocated in Start(), deallocated in Stop()/Abort()
  nsTArray<AudioDataValue> mMonoBuffer;

  // [Atomic] Thread control flag for background thread loop
  std::atomic<bool> mResamplingThreadRunning{false};

  // [Main thread write, Background thread read] Graph sample rate
  // Set once in Initialize(), read by background thread for resampling
  uint32_t mGraphRate = 0;

  // [Main thread write, Background thread read] Session identifier
  std::atomic<uint64_t> mSessionId = 0;

  // Per-instance IPC channel for speech recognition sessions
  RefPtr<mozilla::ipc::SpeechRecognitionChild> mSpeechRecognitionChild;

  // Converts from graph rate to ASR model rate (16kHz)
  UniquePtr<AudioConverter> mAudioConverter;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_SpeechRecognitionBackend_h
