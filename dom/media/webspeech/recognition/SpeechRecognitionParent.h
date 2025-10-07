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

 private:
  ~SpeechRecognitionParent();

  void InitializeWhisperOnBackgroundThread();
  void RetrieveModelBlob();
  void ProcessAudioOnBackgroundThread();
  void CleanupWhisperContext();
  void LoadPreferences();

  // Static tracking of the single active recognition session
  static StaticRefPtr<SpeechRecognitionParent> sActiveSession;
  static StaticMutex sSessionMutex MOZ_UNANNOTATED;
  nsCString mLanguage;
  // Contextual biasing phrases
  nsTArray<nsString> mPhrases;
  // Stream allowing access to model data
  nsCOMPtr<nsIInputStream> mModelStream;
  bool mIsActive;
  RefPtr<SpeechRecognitionMetadataCallback> mMetadataCallback;
  // Model file handle from blob
  FILE* mModelFile = nullptr;
  std::atomic<bool> mWhisperInitPending{false};

  // Whisper-related members
  whisper_context* mWhisperCtx;
  mozilla::llama::LlamaLibWrapper* mLib;
  std::thread mBackgroundThread;
  std::atomic<bool> mThreadRunning;

  // Audio processing members
  mozilla::SPSCQueue<float> mAudioQueue;
  std::vector<float> mAudioRing;
  size_t mRingWritePos;
  size_t mRingSize;

  // Tunable parameters for recognition
  struct RecognitionParams {
    // Latency & Timing
    int32_t mRecognitionIntervalMs = 1000;  // How often to run recognition
    int32_t mAudioLengthMs = 10000;         // Audio segment duration to process
    int32_t mKeepAudioMs = 200;             // Audio overlap between segments
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
    int32_t mMaxTokensPerSegment = 32;      // Max tokens per segment
  };

  RecognitionParams mParams;

  // Dumps audio sent to Whisper. This will contain segments of about 10s of
  // audio, representing the audio sent to whisper.
  WavDumper mWhisperAudioDumper;

  // Continuous recognition members
  size_t mProcessedAudioPos;  // Position in the audio stream that has been processed
  std::vector<int32_t> mPromptTokens;  // Tokens from previous segment for context
  nsCString mAccumulatedTranscript;  // Full transcript accumulation
  nsCString mLastSegmentText;  // Last segment text to detect duplicates
  std::vector<float> mPreviousAudio;  // Audio from previous segment for overlap
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_SpeechRecognitionParent_h
