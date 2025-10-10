/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8  et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONPARENT_H_
#define DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONPARENT_H_

#include <atomic>

#include "WavDumper.h"
#include "mozilla/PSpeechRecognitionParent.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/Promise.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsIThread.h"
#include "nsStringFwd.h"
#include "WavDumper.h"
#include "parakeet.h"
#include "mozilla/FileUtils.h"

namespace mozilla::llama {
struct LlamaLibWrapper;
}

namespace mozilla {

struct ParakeetContextDeleter {
  void operator()(parakeet_context* ctx);
};

class SpeechRecognitionParent final : public PSpeechRecognitionParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SpeechRecognitionParent, override)

  SpeechRecognitionParent();

  ipc::IPCResult RecvIsModelAvailable(
      const nsTArray<nsCString>& aLanguages,
      IsModelAvailableResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInstallModels(
      const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInit(const nsCString& aEngineId,
                                   const nsCString& aLanguage,
                                   const nsTArray<nsString>& aPhrases,
                                   InitResolver&& aResolver);
  mozilla::ipc::IPCResult RecvProcessAudioData(nsTArray<float>&& aAudioData);
  mozilla::ipc::IPCResult RecvStop();

  void ActorDestroy(ActorDestroyReason aReason) override;

  struct ModelIdentifier {
    nsCString mModelName;
    nsCString mFileName;
    nsCString mRevision = "main"_ns;
    nsCString ToString() const;
  };

  ModelIdentifier LanguagesToModelIdentifier(
      const nsTArray<nsCString>& aLanguages);

  void ResolveOrRejectInitOnIPCThread(InitResolver&& aResolver, bool aSuccess)
      MOZ_EXCLUDES(mLock);

 private:
  ~SpeechRecognitionParent();
  void LoadPreferences();

  void InitializeParakeetContext(InitResolver&& aResolver);
  void RetrieveModel(InitResolver&& aResolver);
  void ProcessAudioOnBackgroundThread();
  void SignalError(const nsCString& aErrorMessage);

  parakeet_full_params GetParakeetParams();

  // Static tracking of the single active recognition session
  static StaticMutex sSessionMutex;
  static StaticRefPtr<SpeechRecognitionParent> sActiveSession
      MOZ_GUARDED_BY(sSessionMutex);

  Mutex mLock;
  // Recognition language
  // Set during RecvInit, then constant
  nsCString mLanguage MOZ_GUARDED_BY(mLock);
  // Contextual biasing phrases
  // Set during RecvInit, then constant
  nsTArray<nsString> mPhrases MOZ_GUARDED_BY(mLock);
  // Model file handle - automatically closed on destruction
  // ScopedCloseFile is UniquePtr<FILE, FCloseDeleter>
  mozilla::UniquePtr<FILE, mozilla::FCloseDeleter> mModelFile MOZ_GUARDED_BY(mLock);
  // Parakeet instance. Initialized on the background thread, destroyed after
  // thread has been joined on another thread.
  mozilla::UniquePtr<parakeet_context, mozilla::ParakeetContextDeleter> mParakeetCtx;

  // Lock-free queue to convey audio from the IPC thread to the processing
  // thread. Producer is the IPC thread, consumer is the processing thread.
  mozilla::SPSCQueue<float> mAudioQueue;

  // Started in RecvInit, then stopped and join on actor destroyed, recognitions
  // stopped, etc.
  nsCOMPtr<nsIThread> mRecognitionThread;

  // Tunable parameters for recognition, mapping of whisper's struct into Gecko,
  // bound to prefs.
  struct RecognitionParams {
    // Latency & Timing
    int32_t mRecognitionIntervalMs = 1000;  // How often to run recognition
    int32_t mAudioLengthMs =
        10000;                    // Audio segment duration to process each step
    int32_t mKeepAudioMs = 1000;  // Audio overlap between segments
    int32_t mStepMs = 3000;       // Step size for sliding window mode

    // Recognition Quality
    int32_t mBeamSize = 1;         // Beam search width (1=greedy, >1=beam)
    float mTemperature = 0.0f;     // Sampling temperature
    float mTemperatureInc = 0.2f;  // Temperature increment for fallback
    int32_t mBestOf = 2;           // Candidate count

    // Confidence Thresholds
    float mEntropyThreshold = 2.4f;   // Entropy threshold for decoder
    float mLogProbThreshold = -1.0f;  // Log probability threshold
    float mNoSpeechThreshold = 0.6f;  // No speech detection threshold

    // VAD Parameters -- not wired up yet
    bool mUseVAD = false;               // Enable VAD pre-filtering
    float mVADThreshold = 0.6f;         // VAD activation threshold
    int32_t mVADMinSpeechMs = 250;      // Min speech duration
    int32_t mVADMinSilenceMs = 2000;    // Min silence before cutting
    float mVADEnergyThreshold = 0.01f;  // Energy-based VAD
    int32_t mVADSpeechPadMs = 300;      // Padding around speech

    // Context & Memory
    int32_t mMaxContextTokens =
        224;  // Max tokens for context, 224 is whisper's max
    int32_t mMaxTextContext = 16384;   // Max text context chars
    bool mUseContextCarryover = true;  // Enable context carryover

    // Performance
    int32_t mNumThreads = 4;           // Inference threads when not using GPU
    int32_t mAudioContextSize = 0;     // Whisper audio context (0=full)
    bool mSingleSegment = false;       // Force single segment mode
    int32_t mMaxTokensPerSegment = 0;  // Max tokens per segment (0 = no limit)
  };

  RecognitionParams mParams;

  // Dumps audio sent to Whisper. This will contain segments of about 10s of
  // audio, representing the audio sent to whisper.
  // MOZ_DISABLE_UTILITY_SANDBOX=1 MOZ_DUMP_AUDIO=1 to activate
  WavDumper mWhisperAudioDumper;

  // Flag to signal the recognition thread to stop processing. Set to true when
  // starting, false when we want to stop. Checked periodically by the recognition
  // thread during audio processing.
  std::atomic<bool> mShouldContinueProcessing;

  // Position in the audio stream that has been processed in samples
  // This provides a rather crude timing estimate, but will be improved.
  size_t mProcessedAudioPos;
};

}  // namespace mozilla

#endif // DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONPARENT_H_
