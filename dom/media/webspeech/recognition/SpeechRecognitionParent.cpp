/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionParent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "mozIRemoteLazyInputStream.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/Blob.h"
#include "mozilla/dom/BlobImpl.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/HWInferenceChild.h"
#include "mozilla/ipc/PHWInference.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/llama/LlamaRuntimeLinker.h"
#include "nsDebug.h"
#include "nsGkAtoms.h"
#include "nsIFileStreams.h"
#include "nsNetUtil.h"
#include "nsString.h"
#include "prio.h"
#include "private/pprio.h"

#ifdef XP_WIN
#  include <fcntl.h>
#endif

#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"

namespace mozilla::ipc {

// Static initialization
StaticRefPtr<SpeechRecognitionParent> SpeechRecognitionParent::sActiveSession;
StaticMutex SpeechRecognitionParent::sSessionMutex;

static LazyLogModule gSpeechRecognitionParentLog("SpeechRecognitionParent");
#define LOGV(fmt, ...)                                             \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Verbose, fmt, \
              ##__VA_ARGS__)
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Error, fmt, ##__VA_ARGS__)

static constexpr int32_t DEFAULT_RECOGNITION_INTERVAL_MS = 1000;  // 1 second
static constexpr int32_t DEFAULT_AUDIO_LENGTH_MS =
    10000;  // 10 seconds of audio to analyze
static constexpr int32_t DEFAULT_NUM_THREADS = 4;

// Whisper sampling strategy enum value
static constexpr int WHISPER_SAMPLING_GREEDY = 0;

// Metadata callback for model blob file descriptor retrieval
class SpeechRecognitionMetadataCallback final : public nsIFileMetadataCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  explicit SpeechRecognitionMetadataCallback(SpeechRecognitionParent* aParent)
      : mParent(aParent) {}

  NS_IMETHOD OnFileMetadataReady(nsIAsyncFileMetadata* aObject) override {
    if (mParent) {
      mParent->OnModelMetadataReceived();
    }
    return NS_OK;
  }

 private:
  virtual ~SpeechRecognitionMetadataCallback() = default;
  SpeechRecognitionParent* mParent = nullptr;
};

NS_IMPL_ISUPPORTS(SpeechRecognitionMetadataCallback, nsIFileMetadataCallback)

SpeechRecognitionParent::ModelIdentifier
SpeechRecognitionParent::LanguagesToModelIdentifier(
    const nsTArray<nsCString>& aLanguages) {
  MOZ_ASSERT(!aLanguages.IsEmpty());

  // Map languages to model names for speech recognition
  // en goes to ggml-small.en
  // all other languages fall back to large turbo v3
  nsCString modelName;
  nsCString fileName;
  nsCString revision = "main"_ns;

  // todo while loop, support requesting multiple languages
  const nsCString& firstLang = aLanguages[0];
  if (firstLang.EqualsLiteral("en") || firstLang.EqualsLiteral("en-US")) {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-small.en.bin"_ns;
  } else {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-large-v3-turbo-q8_0.bin"_ns;
  }

  return {modelName, fileName, revision};
}

nsCString SpeechRecognitionParent::ModelIdentifier::ToString() const {
  return nsFmtCString(FMT_STRING("{}/{}/{}"), mModelName.get(), mFileName.get(),
                      mRevision.get());
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvIsModelAvailable(
    const nsTArray<nsCString>& aLanguages,
    IsModelAvailableResolver&& aResolver) {
  LOGD("{} RecvIsModelAvailable called for languages: {}", __func__,
       fmt::join(aLanguages, ", "));

  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);

  // Get HWInferenceChild to check model availability
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} No UtilityProcessChild available", __func__);
    aResolver(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available", __func__);
    aResolver(false);
    return IPC_OK();
  }

  LOGD(
      "{} Sending model availability request to main process, {} "
      "mapped to model={}",
      __func__, fmt::join(aLanguages, ", "), modelIdentifier.ToString().get());

  auto promise = hwInferenceChild->SendIsModelAvailable(
      modelIdentifier.mModelName, modelIdentifier.mRevision,
      modelIdentifier.mFileName);
  promise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self = RefPtr{this}, resolver = aResolver](bool aAvailable) mutable {
        LOGD("Sending response back to content process: available={}",
             aAvailable ? "true" : "false");
        resolver(aAvailable);
      },
      [self = RefPtr{this},
       resolver = aResolver](ResponseRejectReason aReason) mutable {
        LOGE("{} IPC call to main process failed: {}", __func__,
             static_cast<int>(aReason));
        resolver(false);
      });

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInstallModels(
    const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver) {
  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);

  LOGD("[{} Mapped to model: {}, revision: {}, filename: {}", __func__,
       modelIdentifier.mModelName.get(), modelIdentifier.mRevision.get(),
       modelIdentifier.mFileName.get());

  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} No UtilityProcessChild available", __func__);
    aResolver(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available", __func__);
    aResolver(false);
    return IPC_OK();
  }

  LOGD(
      "{} Sending model installation request to main process via "
      "HWInference: model={}",
      __func__, modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendInstallModel(modelIdentifier.mModelName, modelIdentifier.mRevision,
                         modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr(this), aResolver](bool aSuccess) mutable {
            LOGD(
                "{} Received installation response from main process: "
                "success={}",
                __func__, aSuccess ? "true" : "false");
            aResolver(aSuccess);
          },
          [self = RefPtr(this),
           aResolver](ResponseRejectReason aReason) mutable {
            LOGE("{} IPC call to main process failed: {}", __func__,
                 static_cast<int>(aReason));
            aResolver(false);
          });

  return IPC_OK();
}

SpeechRecognitionParent::SpeechRecognitionParent()
    : mIsActive(false),
      mWhisperCtx(nullptr),
      mLib(nullptr),
      mThreadRunning(false),
      mAudioQueue(WHISPER_SAMPLE_RATE * 30),  // 30 seconds of audio buffer
      mRingWritePos(0),
      mRingSize(WHISPER_SAMPLE_RATE * 10),  // Default 10 seconds, will be updated
      mParams() {  // Initialize with defaults
  // Initialize ring buffer
  mAudioRing.resize(mRingSize, 0.0f);

  // MOZ_DUMP_AUDIO=1 MOZ_DISABLE_UTILITY_SANDBOX=1 to activate this
  mWhisperAudioDumper.Open("SpeechRecognition-Whisper-Input", 1,
                           WHISPER_SAMPLE_RATE);

  // Initialize for continuous recognition
  mProcessedAudioPos = 0;

  // Load tunable parameters from preferences (can be overridden via about:config)
  LoadPreferences();
}

void SpeechRecognitionParent::LoadPreferences() {
  // These can be set via about:config for tuning
  // Example: media.webspeech.recognition.interval_ms

  // Timing parameters
  mParams.mRecognitionIntervalMs = Preferences::GetInt(
      "media.webspeech.recognition.interval_ms", 1000);
  mParams.mAudioLengthMs = Preferences::GetInt(
      "media.webspeech.recognition.audio_length_ms", 10000);
  mParams.mKeepAudioMs = Preferences::GetInt(
      "media.webspeech.recognition.keep_audio_ms", 200);
  mParams.mStepMs = Preferences::GetInt(
      "media.webspeech.recognition.step_ms", 3000);

  // Quality parameters
  mParams.mBeamSize = Preferences::GetInt(
      "media.webspeech.recognition.beam_size", 1);
  mParams.mTemperature = Preferences::GetFloat(
      "media.webspeech.recognition.temperature", 0.0f);
  mParams.mTemperatureInc = Preferences::GetFloat(
      "media.webspeech.recognition.temperature_inc", 0.2f);
  mParams.mBestOf = Preferences::GetInt(
      "media.webspeech.recognition.best_of", 2);

  // Thresholds
  mParams.mEntropyThreshold = Preferences::GetFloat(
      "media.webspeech.recognition.entropy_threshold", 2.4f);
  mParams.mLogProbThreshold = Preferences::GetFloat(
      "media.webspeech.recognition.logprob_threshold", -1.0f);
  mParams.mNoSpeechThreshold = Preferences::GetFloat(
      "media.webspeech.recognition.no_speech_threshold", 0.6f);

  // VAD parameters
  mParams.mUseVAD = Preferences::GetBool(
      "media.webspeech.recognition.use_vad", false);
  mParams.mVADThreshold = Preferences::GetFloat(
      "media.webspeech.recognition.vad_threshold", 0.6f);
  mParams.mVADMinSpeechMs = Preferences::GetInt(
      "media.webspeech.recognition.vad_min_speech_ms", 250);
  mParams.mVADMinSilenceMs = Preferences::GetInt(
      "media.webspeech.recognition.vad_min_silence_ms", 2000);

  // Context parameters
  mParams.mMaxContextTokens = Preferences::GetInt(
      "media.webspeech.recognition.max_context_tokens", 224);
  mParams.mUseContextCarryover = Preferences::GetBool(
      "media.webspeech.recognition.use_context", true);

  // Performance parameters
  mParams.mNumThreads = Preferences::GetInt(
      "media.webspeech.recognition.num_threads", 4);
  mParams.mAudioContextSize = Preferences::GetInt(
      "media.webspeech.recognition.audio_context_size", 0);
  mParams.mMaxTokensPerSegment = Preferences::GetInt(
      "media.webspeech.recognition.max_tokens_per_segment", 32);

  // Update ring buffer size if audio length changed
  mRingSize = WHISPER_SAMPLE_RATE * (mParams.mAudioLengthMs / 1000);
  mAudioRing.resize(mRingSize, 0.0f);

  LOGD("Loaded recognition parameters: interval={}ms, length={}ms, beam={}, threads={}",
       mParams.mRecognitionIntervalMs, mParams.mAudioLengthMs,
       mParams.mBeamSize, mParams.mNumThreads);
}

void SpeechRecognitionParent::RetrieveModelBlob() {
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} ERROR: No UtilityProcessChild available", __func__);
    return;
  }
  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available for model blob retrieval", __func__);
    return;
  }

  ModelIdentifier modelIdentifier =
      LanguagesToModelIdentifier(nsTArray{mLanguage});

  LOGD("{} Requesting model blob: model={}", __func__,
       modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendGetModelBlob(modelIdentifier.mModelName, modelIdentifier.mRevision,
                         modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}](
              const mozilla::ipc::GetModelBlobResult& aResult) mutable {
            if (aResult.type() ==
                mozilla::ipc::GetModelBlobResult::TGetModelBlobError) {
              LOGE("{} GetModelBlobError with nsresult={:x}", __func__,
                   static_cast<uint32_t>(
                       aResult.get_GetModelBlobError().errorCode()));
              return;
            }

            const mozilla::dom::IPCBlob& blob =
                aResult.get_GetModelBlobSuccess().blob();
            RefPtr<mozilla::dom::BlobImpl> blobImpl =
                mozilla::dom::IPCBlobUtils::Deserialize(blob);

            if (!blobImpl) {
              LOGE("{} Could not deserialize IPCBlob", __func__);
              return;
            }
            mozilla::ErrorResult errorResult;
            blobImpl->CreateInputStream(getter_AddRefs(self->mModelStream),
                                        errorResult);
            if (errorResult.Failed()) {
              LOGE("{}: CreateInputStream failed", __func__);
              return;
            }

            nsCOMPtr<nsIEventTarget> eventTarget =
                mozilla::GetCurrentSerialEventTarget();
            nsCOMPtr<nsIAsyncFileMetadata> asyncFileMetadata =
                do_QueryInterface(self->mModelStream);
            MOZ_ASSERT(asyncFileMetadata,
                       "Programming error: Stream does not support "
                       "nsIAsyncFileMetadata interface");

            self->mMetadataCallback =
                MakeAndAddRef<SpeechRecognitionMetadataCallback>(self);
            nsresult rv = asyncFileMetadata->AsyncFileMetadataWait(
                self->mMetadataCallback.get(), eventTarget);
            if (NS_WARN_IF(NS_FAILED(rv))) {
              LOGE("{} AsyncFileMetadataWait returned error 0x{:x}", __func__,
                   static_cast<uint32_t>(rv));
              return;
            }
          },
          [self = RefPtr{this}](
              mozilla::ipc::ResponseRejectReason aReason) mutable {
            LOGE("{} Promise rejected with reason {}", __func__,
                 static_cast<int>(aReason));
          });
}

void SpeechRecognitionParent::OnModelMetadataReceived() {
  mMetadataCallback = nullptr;

  const nsCOMPtr<nsIFileMetadata> fileMetadata =
      do_QueryInterface(mModelStream);
  MOZ_ASSERT(
      fileMetadata,
      "Programming error, mModelStream doesn't implement nsIFileMetadata");

  // Get FILE* from mModelStream
  PRFileDesc* fileDesc;
  const nsresult rv = fileMetadata->GetFileDescriptor(&fileDesc);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOGE("{} GetFileDescriptor error, neresult=0x{:x}", __func__,
         static_cast<uint32_t>(rv));
    return;
  }
  MOZ_ASSERT(fileDesc);
#ifdef XP_WIN
  // Convert our file descriptor to FILE*
  void* handle = mozilla::ipc::FileDescriptor::PlatformHandleType(
      PR_FileDesc2NativeHandle(fileDesc));
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY);
  if (fd == -1) {
    LOGE("{} _open_osfhandle failed", __func__);
    return;
  }
#else
  PROsfd fd = PR_FileDesc2NativeHandle(fileDesc);
#endif
  FILE* fp = fdopen(fd, "rb");
  if (!fp) {
    LOGE("{} fdopen failed", __func__);
    return;
  }

  mModelFile = fp;
  mWhisperInitPending.store(true);
}

SpeechRecognitionParent::~SpeechRecognitionParent() {
  LOGD("{} mIsActive={}", __func__, mIsActive ? "true" : "false");

  // Clear active session if this was it
  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session in destructor");
      sActiveSession = nullptr;
    }
  }

  if (mIsActive) {
    LOGD("Destroying active session, cleaning up");
    mIsActive = false;
  }

  if (mThreadRunning.load()) {
    mThreadRunning.store(false);
    if (mBackgroundThread.joinable()) {
      mBackgroundThread.join();
    }
  }

  CleanupWhisperContext();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInit(
    const nsCString& aLanguage, const nsTArray<nsString>& aPhrases,
    InitResolver&& aResolver) {
  LOGD("{} language='{}'", __func__, aLanguage.get());

  // Enforce single active session
  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession) {
      LOGE("Rejecting Init - another recognition session is already active");
      aResolver(false);
      return IPC_OK();
    }
    sActiveSession = this;
    LOGD("Session registered as active");
  }

  mLanguage = aLanguage;
  mPhrases = aPhrases.Clone();
  mIsActive = true;

  // Reset continuous recognition state for new session
  mAccumulatedTranscript.Truncate();
  mLastSegmentText.Truncate();
  mPromptTokens.clear();
  mProcessedAudioPos = 0;

  if (!mThreadRunning.load()) {
    mThreadRunning.store(true);
    mBackgroundThread = std::thread([self = RefPtr{this}]() {
      self->InitializeWhisperOnBackgroundThread();
      self->ProcessAudioOnBackgroundThread();
    });
  }

  // Since we're using blob-based loading, we'll initialize in the background
  // thread For now, just resolve success
  aResolver(true);
  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvProcessAudioData(
    nsTArray<float>&& aAudioData) {
  LOGV("{} {} samples", __func__, aAudioData.Length());

  if (!mIsActive) {
    LOGD("Received audio data but session is not active, ignoring");
    return IPC_OK();
  }

  if (!mAudioQueue.Enqueue(aAudioData.Elements(), (int)aAudioData.Length())) {
    LOGD("Audio queue full, dropping sample");
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvStop() {
  LOGD("RecvStop called, mIsActive={}", mIsActive ? "true" : "false");

  // Clear active session if this was it
  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session in RecvStop");
      sActiveSession = nullptr;
    }
  }

  if (mIsActive) {
    mIsActive = false;

    // Send final result with accumulated transcript
    if (!mAccumulatedTranscript.IsEmpty() && CanSend()) {
      LOGD("Sending final transcript: '{}'", mAccumulatedTranscript.get());
      Unused << SendOnRecognitionResult(mAccumulatedTranscript, true);  // true = final
    }

    // Stop background thread
    if (mThreadRunning.load()) {
      mThreadRunning.store(false);
      LOGD("Signaled background thread to stop");
    }

    LOGD("Stopping speech recognition session and cleaning up resources");
  } else {
    LOGD("Stop called on inactive session");
  }
  return IPC_OK();
}

void SpeechRecognitionParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("ActorDestroy called, reason={}, mIsActive={}",
       static_cast<int>(aReason), mIsActive ? "true" : "false");

  if (mIsActive) {
    LOGD("Actor destroyed while session was active, cleaning up");
    mIsActive = false;
  }

  // Stop background thread
  if (mThreadRunning.load()) {
    mThreadRunning.store(false);
    if (mBackgroundThread.joinable()) {
      mBackgroundThread.join();
    }
  }
}

void SpeechRecognitionParent::InitializeWhisperOnBackgroundThread() {
  if (!mLib) {
    mLib = mozilla::llama::LlamaRuntimeLinker::Get();
    if (!mLib) {
      LOGE("{} Failed to get runtime linker", __func__);
      return;
    }
  }

  RefPtr<SpeechRecognitionParent> self = this;
  NS_DispatchToMainThread(
      NS_NewRunnableFunction("SpeechRecognitionParent::RetrieveModelBlob",
                             [self]() { self->RetrieveModelBlob(); }));
}

void SpeechRecognitionParent::ProcessAudioOnBackgroundThread() {
  LOGD("{} Starting continuous recognition loop", __func__);

  std::vector<float> audioForRecognition;
  std::vector<float> audioBuffer;  // Continuous audio buffer

  // Two background tasks: audio ring buffer management and periodic recognition
  std::thread audioConsumerThread([self = RefPtr{this}]() {
    while (self->mThreadRunning.load()) {
      // Continuously dequeue audio and add to ring buffer
      size_t available = self->mAudioQueue.AvailableRead();
      if (available > 0) {
        // Process available audio in chunks
        size_t samples_to_process =
            std::min(available, static_cast<size_t>(1024));
        std::vector<float> samples(samples_to_process);
        size_t dequeued =
            self->mAudioQueue.Dequeue(samples.data(), samples_to_process);

        // Add samples to ring buffer
        for (size_t i = 0; i < dequeued; i++) {
          self->mAudioRing[self->mRingWritePos] = samples[i];
          self->mRingWritePos = (self->mRingWritePos + 1) % self->mRingSize;
        }

        LOGV("Added {} samples to ring buffer, writePos={}", dequeued, self->mRingWritePos);
      }

      // Small sleep to prevent excessive CPU usage
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  // Main recognition loop - runs every recognition interval
  auto lastRecognitionTime = std::chrono::steady_clock::now();

  while (mThreadRunning.load()) {
    // Check if we need to initialize Whisper with the FILE* handle
    if (mWhisperInitPending.load() && mModelFile && !mWhisperCtx) {
      struct whisper_context_params cparams =
          mLib->whisper_context_default_params();

#ifdef XP_MACOSX
      cparams.use_gpu = true;
#else
      cparams.use_gpu = false;
#endif

      mWhisperCtx =
          mLib->whisper_init_from_file_handle_with_params(mModelFile, cparams);

      if (!mWhisperCtx) {
        LOGE(
            "{} ERROR whisper_init_from_file_handle_with_params "
            "returned nullptr",
            __func__);
        fclose(mModelFile);
        mModelFile = nullptr;
      } else {
        LOGD("{} Got Whisper context", __func__);
      }

      mWhisperInitPending.store(false);
    }

    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastRecognition =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            currentTime - lastRecognitionTime)
            .count();

    if (timeSinceLastRecognition >= mParams.mRecognitionIntervalMs) {
      // Time for recognition - extract audio from ring buffer
      size_t samples_to_analyze = std::min(
          mRingSize,
          static_cast<size_t>((1e-3 * mParams.mAudioLengthMs) * WHISPER_SAMPLE_RATE));

      // Calculate samples to keep from previous recognition (for context/overlap)
      const size_t n_samples_keep = static_cast<size_t>((1e-3 * mParams.mKeepAudioMs) * WHISPER_SAMPLE_RATE);

      audioForRecognition.clear();

      // First, add kept samples from previous recognition if available
      if (!mPreviousAudio.empty() && n_samples_keep > 0) {
        size_t samples_to_keep = std::min(n_samples_keep, mPreviousAudio.size());
        size_t start_idx = mPreviousAudio.size() - samples_to_keep;
        audioForRecognition.insert(audioForRecognition.end(),
                                  mPreviousAudio.begin() + start_idx,
                                  mPreviousAudio.end());
      }

      // Then add new samples from ring buffer
      size_t new_samples_needed = samples_to_analyze - audioForRecognition.size();
      size_t start_pos = audioForRecognition.size();
      audioForRecognition.resize(samples_to_analyze);

      for (size_t i = 0; i < new_samples_needed; i++) {
        size_t ring_pos =
            (mRingWritePos + mRingSize - new_samples_needed + i) % mRingSize;
        audioForRecognition[start_pos + i] = mAudioRing[ring_pos];
      }

      // Store current audio for next iteration
      mPreviousAudio = audioForRecognition;

      LOGV("{} Running recognition on {} samples ({:.2f}s of audio)",
           __func__, samples_to_analyze,
           samples_to_analyze / (float)WHISPER_SAMPLE_RATE);

      // Dump audio data that will be sent to Whisper for debugging
      mWhisperAudioDumper.Write(audioForRecognition.data(),
                                audioForRecognition.size());

      if (mWhisperCtx && audioForRecognition.size() > 0 && mLib) {
        // Run Whisper inference with configurable parameters
        enum whisper_sampling_strategy strategy = static_cast<enum whisper_sampling_strategy>(
            mParams.mBeamSize > 1
            ? WHISPER_SAMPLING_BEAM_SEARCH
            : WHISPER_SAMPLING_GREEDY);
        whisper_full_params wparams = mLib->whisper_full_default_params(strategy);

        // Basic settings
        wparams.print_progress = false;
        wparams.print_special = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = true;
        wparams.translate = false;
        wparams.single_segment = mParams.mSingleSegment;
        wparams.max_tokens = mParams.mMaxTokensPerSegment;
        wparams.language = mLanguage.get();
        wparams.n_threads = mParams.mNumThreads;
        wparams.audio_ctx = mParams.mAudioContextSize;

        // Quality parameters
        wparams.temperature = mParams.mTemperature;
        wparams.temperature_inc = mParams.mTemperatureInc;
        wparams.beam_search.beam_size = mParams.mBeamSize;
        wparams.greedy.best_of = mParams.mBestOf;

        // Confidence thresholds
        wparams.entropy_thold = mParams.mEntropyThreshold;
        wparams.logprob_thold = mParams.mLogProbThreshold;
        wparams.no_speech_thold = mParams.mNoSpeechThreshold;

        // Build prompt from phrases and previous context
        nsCString prompt;
        for (auto& phrase : mPhrases) {
          prompt.Append(NS_ConvertUTF16toUTF8(phrase));
          prompt.AppendLiteral(". ");
        }
        wparams.initial_prompt = prompt.get();

        // Use tokens from previous segment as context for continuity
        if (mParams.mUseContextCarryover) {
          wparams.prompt_tokens = mPromptTokens.empty() ? nullptr : mPromptTokens.data();
          wparams.prompt_n_tokens = mPromptTokens.size();
          wparams.no_context = false;  // Keep context for continuous recognition
        } else {
          wparams.prompt_tokens = nullptr;
          wparams.prompt_n_tokens = 0;
          wparams.no_context = true;
        }

        if (mLib->whisper_full(mWhisperCtx, wparams, audioForRecognition.data(),
                               audioForRecognition.size()) == 0) {
          // Process results
          const int n_segments = mLib->whisper_full_n_segments(mWhisperCtx);

          // Clear previous prompt tokens to build new ones
          mPromptTokens.clear();

          // Process each segment
          for (int i = 0; i < n_segments; ++i) {
            const char* text =
                mLib->whisper_full_get_segment_text(mWhisperCtx, i);

            if (text && strlen(text) > 0) {
              nsCString segmentText(text);

              // Trim whitespace
              segmentText.Trim(" \t\n\r");

              // Skip if this is a duplicate of the last segment
              if (!segmentText.IsEmpty() && !segmentText.Equals(mLastSegmentText)) {
                // Add to accumulated transcript with proper spacing
                if (!mAccumulatedTranscript.IsEmpty()) {
                  mAccumulatedTranscript.AppendLiteral(" ");
                }
                mAccumulatedTranscript.Append(segmentText);
                mLastSegmentText = segmentText;

                // Collect tokens from this segment for context in next recognition
                const int token_count = mLib->whisper_full_n_tokens(mWhisperCtx, i);
                for (int j = 0; j < token_count; ++j) {
                  mPromptTokens.push_back(mLib->whisper_full_get_token_id(mWhisperCtx, i, j));
                }

                LOGV("{} New segment: '{}', Total transcript: '{}'", __func__,
                     segmentText.get(), mAccumulatedTranscript.get());

                // Send the accumulated transcript (not just the segment)
                bool isFinal = false;  // In continuous mode, never mark as final until stop

                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionParent::SendResult",
                    [self = RefPtr{this},
                     transcript = nsCString(mAccumulatedTranscript),
                     isFinal]() {
                      if (self->CanSend()) {
                        Unused
                            << self->SendOnRecognitionResult(transcript, isFinal);
                      }
                    }));
              } else if (!segmentText.IsEmpty()) {
                LOGV("{} Skipping duplicate segment: '{}'", __func__, segmentText.get());
              }
            }
          }

          // Keep only the last N tokens for context (to avoid growing indefinitely)
          if (mParams.mUseContextCarryover &&
              mPromptTokens.size() > static_cast<size_t>(mParams.mMaxContextTokens)) {
            mPromptTokens.erase(mPromptTokens.begin(),
                               mPromptTokens.begin() + (mPromptTokens.size() - mParams.mMaxContextTokens));
          }
        } else {
          LOGD("Whisper inference failed");
        }
      }

      lastRecognitionTime = currentTime;
    }

    // Sleep until next recognition time
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Wait for audio consumer thread to finish
  if (audioConsumerThread.joinable()) {
    audioConsumerThread.join();
  }

  LOGD("Continuous audio processing loop terminated");
}

void SpeechRecognitionParent::CleanupWhisperContext() {
  if (mWhisperCtx && mLib) {
    mLib->whisper_free(mWhisperCtx);
    mWhisperCtx = nullptr;
  }

  if (mModelFile) {
    fclose(mModelFile);
    mModelFile = nullptr;
  }
}

}  // namespace mozilla::ipc

#undef LOGV
#undef LOGD
#undef LOGE
