/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_SpeechRecognitionParent_h
#define mozilla_ipc_SpeechRecognitionParent_h

#include <atomic>
#include <thread>
#include <vector>

#include "WavDumper.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/PSpeechRecognitionParent.h"
#include "nsCOMPtr.h"
#include "nsIFileStreams.h"
#include "nsIInputStream.h"
#include "nsISupportsImpl.h"
#include "nsIThread.h"
#include "nsStringFwd.h"

struct whisper_context;

namespace mozilla::llama {
struct LlamaLibWrapper;
}

namespace mozilla::ipc {

class SpeechRecognitionMetadataCallback;

class SpeechRecognitionParent final : public PSpeechRecognitionParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(SpeechRecognitionParent)

  SpeechRecognitionParent();

  mozilla::ipc::IPCResult RecvIsModelAvailable(
      const nsTArray<nsCString>& aLanguages,
      IsModelAvailableResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInstallModels(
      const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInit(const nsCString& aLanguage,
                                   const nsTArray<nsString>& aPhrases,
                                   InitResolver&& aResolver);
  mozilla::ipc::IPCResult RecvProcessAudioData(nsTArray<float>&& aAudioData);
  mozilla::ipc::IPCResult RecvStop();

  void ActorDestroy(ActorDestroyReason aReason) override;

  // Called when model blob metadata is ready
  void OnModelMetadataReceived();

  struct ModelIdentifier {
    nsCString mModelName;
    nsCString mFileName;
    nsCString mRevision = "main"_ns;
    nsCString ToString() const;
  };

  ModelIdentifier LanguagesToModelIdentifier(
      const nsTArray<nsCString>& aLanguages);

  void ResolveOrRejectInitOnIPCThread(bool aSuccess);

 private:
  ~SpeechRecognitionParent();

  void InitializeWhisperOnBackgroundThread();
  void RetrieveModelBlob();
  void ProcessAudioOnBackgroundThread();
  void LoadPreferences();

  // Static tracking of the single active recognition session
  static StaticMutex sSessionMutex;
  static StaticRefPtr<SpeechRecognitionParent> sActiveSession MOZ_GUARDED_BY(sSessionMutex);

  Mutex mLock;
  // Recognition language
  // Set during RecvInit, then constant
  nsCString mLanguage ;//MOZ_GUARDED_BY(mLock);
  // Contextual biasing phrases
  // Set during RecvInit, then constant
  nsTArray<nsString> mPhrases; //MOZ_GUARDED_BY(mLock);
  // Only used during init, main thread
  // Stream allowing access to model data
  nsCOMPtr<nsIInputStream> mModelStream; // MOZ_GUARDED_BY(mLock);
  // Callback to receive metadata about the model file, required to then get its
  // underlying file descriptor.
  RefPtr<SpeechRecognitionMetadataCallback> mMetadataCallback; // MOZ_GUARDED_BY(mLock);
  // Model file handle from blob -- closed
  FILE* mModelFile /* MOZ_GUARDED_BY(mLock) */ = nullptr ;
  // Dynamic linker pointer to the library containing whisper functions.
  mozilla::llama::LlamaLibWrapper* mLib;
  // Whisper instance. Initialized on the background thread, destroyed after
  // thread has been joined on another thread.
  whisper_context* mWhisperCtx;
  InitResolver mInitResolver;

  // Lock-free queue to convey audio from the IPC thread to the processing
  // thread. Producer is the IPC thread, consumer is the processing thread.
  mozilla::SPSCQueue<float> mAudioQueue;

  // Started in RecvInit, then stopped and join on actor destroyed, recognitions
  // stopped, etc.
  nsCOMPtr<nsIThread> mRecognitionThread;
  // Atomic that allows telling the thread it needs to exits.
  std::atomic<bool> mThreadRunning;

  // Tunable parameters for recognition
  struct RecognitionParams {
    // Latency & Timing
    int32_t mRecognitionIntervalMs = 1000;  // How often to run recognition
    int32_t mAudioLengthMs = 10000;         // Audio segment duration to process
    int32_t mKeepAudioMs = 1000;            // Audio overlap between segments (more context like CLI)
    int32_t mStepMs = 3000;                 // Step size for sliding window mode

    // Recognition Quality
    int32_t mBeamSize = 1;                  // Beam search width (1=greedy, >1=beam)
    float mTemperature = 0.0f;              // Sampling temperature
    float mTemperatureInc = 0.2f;           // Temperature increment for fallback
    int32_t mBestOf = 2;                    // Best of N candidates

    // Confidence Thresholds
    float mEntropyThreshold = 2.4f;         // Entropy threshold for decoder
    float mLogProbThreshold = -1.0f;        // Log probability threshold
    float mNoSpeechThreshold = 0.6f;        // No speech detection threshold
    float mCompressionRatioThreshold = 2.4f; // Compression ratio threshold

    // VAD Parameters
    bool mUseVAD = false;                   // Enable VAD pre-filtering
    float mVADThreshold = 0.6f;             // VAD activation threshold
    int32_t mVADMinSpeechMs = 250;          // Min speech duration
    int32_t mVADMinSilenceMs = 2000;        // Min silence before cutting
    float mVADEnergyThreshold = 0.01f;      // Energy-based VAD
    int32_t mVADSpeechPadMs = 300;          // Padding around speech

    // Context & Memory
    int32_t mMaxContextTokens = 224;        // Max tokens for context
    int32_t mMaxTextContext = 16384;        // Max text context chars
    bool mUseContextCarryover = true;       // Enable context carryover

    // Performance
    int32_t mNumThreads = 4;                // Inference threads
    int32_t mAudioContextSize = 0;          // Whisper audio context (0=full)
    bool mSingleSegment = false;            // Force single segment mode
    int32_t mMaxTokensPerSegment = 0;       // Max tokens per segment (0 = no limit)
  };

  RecognitionParams mParams;

  // Dumps audio sent to Whisper. This will contain segments of about 10s of
  // audio, representing the audio sent to whisper.
  WavDumper mWhisperAudioDumper;

  // Continuous recognition members
  size_t mProcessedAudioPos;  // Position in the audio stream that has been processed
  // Tokens from previous segment for context, only used when prompt carryover has been enabled.
  std::vector<int32_t> mPromptTokens;
  // Token-level streaming merge state
  std::vector<int32_t> mGroupTokens;       // current group's best tokens
  std::vector<int32_t> mLastFinalTokens;   // tokens of last committed group

  // Audio buffer for keeping previous samples for context
  std::vector<float> mPreviousAudio;
  // Accumulated transcript for the session
  nsCString mAccumulatedTranscript;
  // Last segment text to avoid duplicates
  nsCString mLastSegmentText;
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_SpeechRecognitionParent_h
