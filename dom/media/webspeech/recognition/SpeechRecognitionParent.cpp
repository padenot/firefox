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
#include <ratio>
#include <thread>

#include "mozIRemoteLazyInputStream.h"
#include "mozilla/Logging.h"
#include "mozilla/Mutex.h"
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
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "prio.h"
#include "private/pprio.h"
#include "whisper.h"

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

// Use whisper.h enum values directly for sampling strategy.

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

void SpeechRecognitionParent::ResolveOrRejectInitOnIPCThread(bool aSuccess) {
  if (GetActorEventTarget()->IsOnCurrentThread()) {
    LOGV("Resolving init on same thread {}", aSuccess);
    mInitResolver(aSuccess);
    mInitResolver = nullptr;
  } else {
    LOGV("Resolving init accross thread {}", aSuccess);
    GetActorEventTarget()->Dispatch(NS_NewRunnableFunction(
        "Speech recognition init runnable",
        [resolver = std::move(mInitResolver), aSuccess]() {
          LOGV("Resolving init accross thread {}", aSuccess);
          resolver(aSuccess);
        }));
  }
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
    ResolveOrRejectInitOnIPCThread(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available", __func__);
    ResolveOrRejectInitOnIPCThread(false);
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
      [self = RefPtr{this}](bool aAvailable) mutable {
        LOGD("Sending response back to content process: available={}",
             aAvailable ? "true" : "false");
        self->ResolveOrRejectInitOnIPCThread(aAvailable);
      },
      [self = RefPtr{this}](ResponseRejectReason aReason) mutable {
        LOGE("{} IPC call to main process failed: {}", __func__,
             static_cast<int>(aReason));
        self->ResolveOrRejectInitOnIPCThread(false);
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
    : mLock("SpeechRecognitionLock"),
      mLib(nullptr),
      mWhisperCtx(nullptr),
      // We expect that in some less powerful computer that aren't doing hw
      // accelerated recognition, having a very long queue can smooth things
      // out.
      mAudioQueue(WHISPER_SAMPLE_RATE * 30),
      mThreadRunning(false),
      mParams() {  // Initialize with defaults

  // MOZ_DUMP_AUDIO=1 MOZ_DISABLE_UTILITY_SANDBOX=1 to activate this
  mWhisperAudioDumper.Open("SpeechRecognition-Whisper-Input", 1,
                           WHISPER_SAMPLE_RATE);

  mProcessedAudioPos = 0;

  // Load tunable parameters from preferences (can be overridden via
  // about:config)
  LoadPreferences();
}

void SpeechRecognitionParent::LoadPreferences() {
  // Timing parameters
  mParams.mRecognitionIntervalMs =
      Preferences::GetInt("media.webspeech.recognition.interval_ms", 500);
  mParams.mAudioLengthMs =
      Preferences::GetInt("media.webspeech.recognition.audio_length_ms", 10000);
  mParams.mKeepAudioMs =
      Preferences::GetInt("media.webspeech.recognition.keep_audio_ms", 200);
  mParams.mStepMs =
      Preferences::GetInt("media.webspeech.recognition.step_ms", 3000);

  // Quality parameters
  mParams.mBeamSize =
      Preferences::GetInt("media.webspeech.recognition.beam_size", 1);
  mParams.mTemperature =
      Preferences::GetFloat("media.webspeech.recognition.temperature", 0.0f);
  mParams.mTemperatureInc = Preferences::GetFloat(
      "media.webspeech.recognition.temperature_inc", 0.2f);
  mParams.mBestOf =
      Preferences::GetInt("media.webspeech.recognition.best_of", 2);

  // Thresholds
  mParams.mEntropyThreshold = Preferences::GetFloat(
      "media.webspeech.recognition.entropy_threshold", 2.4f);
  mParams.mLogProbThreshold = Preferences::GetFloat(
      "media.webspeech.recognition.logprob_threshold", -1.0f);
  mParams.mNoSpeechThreshold = Preferences::GetFloat(
      "media.webspeech.recognition.no_speech_threshold", 0.6f);

  // VAD parameters (not wired up yet)
  mParams.mUseVAD =
      Preferences::GetBool("media.webspeech.recognition.use_vad", false);
  mParams.mVADThreshold =
      Preferences::GetFloat("media.webspeech.recognition.vad_threshold", 0.6f);
  mParams.mVADMinSpeechMs =
      Preferences::GetInt("media.webspeech.recognition.vad_min_speech_ms", 250);
  mParams.mVADMinSilenceMs = Preferences::GetInt(
      "media.webspeech.recognition.vad_min_silence_ms", 2000);

  // Context parameters. 224 is a constant in whisper models
  mParams.mMaxContextTokens = Preferences::GetInt(
      "media.webspeech.recognition.max_context_tokens", 224);
  mParams.mUseContextCarryover =
      Preferences::GetBool("media.webspeech.recognition.use_context", true);

  // Performance parameters
  // Not used when using GPU -- a single thread is used for submitting work to
  // the GPU
  mParams.mNumThreads =
      Preferences::GetInt("media.webspeech.recognition.num_threads", 4);
  mParams.mAudioContextSize =
      Preferences::GetInt("media.webspeech.recognition.audio_context_size", 0);
  mParams.mMaxTokensPerSegment = Preferences::GetInt(
      "media.webspeech.recognition.max_tokens_per_segment", 0);
}

void SpeechRecognitionParent::RetrieveModelBlob() {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} ERROR: No UtilityProcessChild available", __func__);
    ResolveOrRejectInitOnIPCThread(false);
    return;
  }
  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available for model blob retrieval", __func__);
    ResolveOrRejectInitOnIPCThread(false);
    return;
  }

  // MutexAutoLock lock(mLock);
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
            // MutexAutoLock lock(self->mLock);
            if (aResult.type() ==
                mozilla::ipc::GetModelBlobResult::TGetModelBlobError) {
              LOGE("{} GetModelBlobError with nsresult={:x}", __func__,
                   static_cast<uint32_t>(
                       aResult.get_GetModelBlobError().errorCode()));
              self->ResolveOrRejectInitOnIPCThread(false);
              return;
            }

            const mozilla::dom::IPCBlob& blob =
                aResult.get_GetModelBlobSuccess().blob();
            RefPtr<mozilla::dom::BlobImpl> blobImpl =
                mozilla::dom::IPCBlobUtils::Deserialize(blob);

            if (!blobImpl) {
              LOGE("{} Could not deserialize IPCBlob", __func__);
              self->ResolveOrRejectInitOnIPCThread(false);
              return;
            }
            mozilla::ErrorResult errorResult;
            blobImpl->CreateInputStream(getter_AddRefs(self->mModelStream),
                                        errorResult);
            if (errorResult.Failed()) {
              LOGE("{}: CreateInputStream failed", __func__);
              self->ResolveOrRejectInitOnIPCThread(false);
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
              self->ResolveOrRejectInitOnIPCThread(false);
              return;
            }
            // In case of success, next steps in `OnModelMetadataReceived`
            // just below
          },
          [self = RefPtr{this}](
              mozilla::ipc::ResponseRejectReason aReason) mutable {
            LOGE("{} Promise rejected with reason {}", __func__,
                 static_cast<int>(aReason));
            self->ResolveOrRejectInitOnIPCThread(false);
          });
}

void SpeechRecognitionParent::OnModelMetadataReceived() {
  // MutexAutoLock lock(mLock);
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
    ResolveOrRejectInitOnIPCThread(false);
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
    ResolveOrRejectOnIPCThread(false);
    return;
  }
#else
  PROsfd fd = PR_FileDesc2NativeHandle(fileDesc);
#endif
  FILE* fp = fdopen(fd, "rb");
  if (!fp) {
    LOGE("{} fdopen failed", __func__);
    ResolveOrRejectInitOnIPCThread(false);
    return;
  }

  mModelFile = fp;

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
    LOGE("{} whisper_init_from_file_handle_with_params failed", __func__);
    fclose(mModelFile);
    mModelFile = nullptr;
    ResolveOrRejectInitOnIPCThread(false);
    return;
  }

  ResolveOrRejectInitOnIPCThread(true);
  LOGD("Whisper context ready, starting main recognition loop");

  // MutexAutoUnlock unlock(mLock);
  mRecognitionThread->Dispatch(NS_NewRunnableFunction(
      "Whisper recognition loop",
      [self = RefPtr{this}] { self->ProcessAudioOnBackgroundThread(); }));
}

SpeechRecognitionParent::~SpeechRecognitionParent() {
  LOGD("{}", __func__);

  // Clear active session if this was it
  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session in destructor");
      sActiveSession = nullptr;
    }
  }

  mRecognitionThread->Shutdown();

  if (mWhisperCtx && mLib) {
    mLib->whisper_free(mWhisperCtx);
    mWhisperCtx = nullptr;
  }

  // MutexAutoLock lock(mLock);
  if (mModelFile) {
    fclose(mModelFile);
    mModelFile = nullptr;
  }
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

  // MutexAutoLock lock(mLock);
  mLanguage = aLanguage;
  mPhrases = aPhrases.Clone();

  mPromptTokens.clear();
  mProcessedAudioPos = 0;
  mGroupTokens.clear();
  mLastFinalTokens.clear();

  // What follows is a long chain of asynchronous steps within and outside of
  // this process. Stash the resolver here to properly call it when it's time.
  mInitResolver = aResolver;

  // We perform the initialization on the same thread we'll later use for the
  // main recognition loop.
  MOZ_ASSERT(!mThreadRunning.load());
  mThreadRunning.store(true);

  nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
      "Whisper init",
      [self = RefPtr{this}]() { self->InitializeWhisperOnBackgroundThread(); });
  nsresult rv = NS_NewNamedThread("Whisper", getter_AddRefs(mRecognitionThread),
                                  runnable.forget());
  if (NS_FAILED(rv)) {
    LOGE("Failed to create recognition thread: {:x}",
         static_cast<uint32_t>(rv));
    mThreadRunning.store(false, std::memory_order_release);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvProcessAudioData(
    nsTArray<float>&& aAudioData) {
  LOGV("{} {} samples", __func__, aAudioData.Length());

  if (!mAudioQueue.Enqueue(aAudioData.Elements(), (int)aAudioData.Length())) {
    LOGD("Audio queue full, dropping sample");
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvStop() {
  // Clear active session if this was it
  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session in RecvStop");
      sActiveSession = nullptr;
    }
  }

  // Stop background thread
  if (mThreadRunning.load()) {
    mThreadRunning.store(false);
    LOGD("Signaled background thread to stop");
  }

  LOGD("Stopping speech recognition session and cleaning up resources");
  return IPC_OK();
}

void SpeechRecognitionParent::ActorDestroy(ActorDestroyReason aReason) {
  mThreadRunning = false;
  mRecognitionThread->Shutdown();
}

void SpeechRecognitionParent::InitializeWhisperOnBackgroundThread() {
  if (!mLib) {
    mLib = mozilla::llama::LlamaRuntimeLinker::Get();
    if (!mLib) {
      LOGE("{} Failed to get runtime linker", __func__);
      ResolveOrRejectInitOnIPCThread(false);
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

  // Similar to cubeb-transcript.cpp setup
  const int n_samples_step =
      (int)((1e-3 * mParams.mStepMs) * WHISPER_SAMPLE_RATE);
  const int n_samples_len =
      (int)((1e-3 * mParams.mAudioLengthMs) * WHISPER_SAMPLE_RATE);
  const int n_samples_keep =
      std::min((int)((1e-3 * mParams.mKeepAudioMs) * WHISPER_SAMPLE_RATE),
               n_samples_len);

  // Calculate number of iterations before new line (similar to
  // cubeb-transcript)
  const int n_new_line =
      std::max(1, mParams.mAudioLengthMs / mParams.mStepMs - 1);
  int n_iter = 0;

  std::vector<float> pcmf32(n_samples_len, 0.0f);
  std::vector<float> pcmf32_old;
  std::vector<float> pcmf32_new;

  // Current line's accumulated transcript
  nsCString currentLineTranscript;
  // Last segment text to avoid duplicates
  nsCString lastSegmentText;

  nsCString language = mLanguage;
  nsCString prompt;
  for (const auto& phrase : mPhrases) {
    prompt.Append(NS_ConvertUTF16toUTF8(phrase));
    prompt.AppendLiteral(". ");
  }

  auto lastRecognitionTime = std::chrono::steady_clock::now();

  while (mThreadRunning.load()) {
    // Step 1: Dequeue audio from SPSC (mimicking cubeb-transcript line 741)
    int available = mAudioQueue.AvailableRead();
    if (available < n_samples_step) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Dequeue new audio
    pcmf32_new.resize(n_samples_step);
    size_t dequeued = mAudioQueue.Dequeue(pcmf32_new.data(), n_samples_step);
    if (dequeued < (size_t)n_samples_step) {
      pcmf32_new.resize(dequeued);
    }

    // Check timing
    auto now = std::chrono::steady_clock::now();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastRecognitionTime)
            .count();
    if (elapsedMs < mParams.mRecognitionIntervalMs) {
      // Not time yet for recognition
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    // Step 2: Build audio buffer (similar to cubeb-transcript lines 769-780)
    const int n_samples_new = pcmf32_new.size();

    // Take up to keep_ms audio from previous iteration
    const int n_samples_take =
        std::min((int)pcmf32_old.size(),
                 std::max(0, n_samples_keep + n_samples_len - n_samples_new));

    pcmf32.resize(n_samples_new + n_samples_take);

    // Copy old samples that we're keeping
    for (int i = 0; i < n_samples_take; i++) {
      pcmf32[i] = pcmf32_old[pcmf32_old.size() - n_samples_take + i];
    }

    // Add new samples
    memcpy(pcmf32.data() + n_samples_take, pcmf32_new.data(),
           n_samples_new * sizeof(float));

    pcmf32_old = pcmf32;

    // Dump audio for debugging
    mWhisperAudioDumper.Write(pcmf32.data(), pcmf32.size());

    // Step 3: Configure and run whisper inference
    enum whisper_sampling_strategy strat =
        static_cast<enum whisper_sampling_strategy>(
            (mParams.mBeamSize > 1) ? WHISPER_SAMPLING_BEAM_SEARCH
                                    : WHISPER_SAMPLING_GREEDY);
    whisper_full_params wparams = mLib->whisper_full_default_params(strat);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = true;
    wparams.translate = false;
    wparams.single_segment = mParams.mSingleSegment;
    wparams.max_tokens =
        mParams.mMaxTokensPerSegment;  // 0 = unlimited (recommended)
    wparams.language = language.get();
    wparams.n_threads = mParams.mNumThreads;
    wparams.audio_ctx = mParams.mAudioContextSize;

    wparams.temperature = mParams.mTemperature;
    wparams.temperature_inc = mParams.mTemperatureInc;
    wparams.beam_search.beam_size = mParams.mBeamSize;
    wparams.greedy.best_of = mParams.mBestOf;
    wparams.entropy_thold = mParams.mEntropyThreshold;
    wparams.logprob_thold = mParams.mLogProbThreshold;
    wparams.no_speech_thold = mParams.mNoSpeechThreshold;

    wparams.initial_prompt = prompt.IsEmpty() ? nullptr : prompt.get();

    if (mParams.mUseContextCarryover) {
      wparams.prompt_tokens =
          mPromptTokens.empty() ? nullptr : mPromptTokens.data();
      wparams.prompt_n_tokens = (int)mPromptTokens.size();
      wparams.no_context = false;
    } else {
      wparams.prompt_tokens = nullptr;
      wparams.prompt_n_tokens = 0;
      wparams.no_context = true;
    }

    if (!(mWhisperCtx && mLib->whisper_full(mWhisperCtx, wparams, pcmf32.data(),
                                            (int)pcmf32.size()) == 0)) {
      LOGD("whisper_full failed or context not ready");
      // Back-off briefly to avoid busy loop
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    // Step 4: Process results
    const int n_segments = mLib->whisper_full_n_segments(mWhisperCtx);
    bool appendedAnything = false;

    for (int i = 0; i < n_segments; ++i) {
      const char* text = mLib->whisper_full_get_segment_text(mWhisperCtx, i);
      if (!text || !text[0]) {
        continue;
      }
      nsCString segmentText(text);
      segmentText.Trim(" \t\n\r");

      // Skip empty or duplicate segments
      if (segmentText.IsEmpty() || segmentText.Equals(lastSegmentText)) {
        continue;
      }

      // Append to current line
      if (!currentLineTranscript.IsEmpty()) {
        currentLineTranscript.AppendLiteral(" ");
      }
      currentLineTranscript.Append(segmentText);
      lastSegmentText = segmentText;
      appendedAnything = true;
    }

    // Increment iteration counter first
    ++n_iter;

    // Check for new line (similar to cubeb-transcript line 942)
    bool isNewLine = (n_iter % n_new_line) == 0;

    // Send results if we have new content
    if (appendedAnything && !currentLineTranscript.IsEmpty()) {
      // Send as FINAL if this is the end of a line, INTERIM otherwise
      bool isFinal = isNewLine;

      nsCString toSend = currentLineTranscript;
      NS_DispatchToMainThread(NS_NewRunnableFunction(
          "SpeechRecognitionParent::SendResult",
          [self = RefPtr{this}, payload = toSend, isFinal]() {
            if (self->CanSend()) {
              LOGV("Sending result: '{}' (final={})", payload.get(), isFinal);
              Unused << self->SendOnRecognitionResult(payload, isFinal);
            }
          }));
    }

    // If new line detected, clear transcript for next line
    if (isNewLine) {
      LOGD("New line detected at iteration {}, clearing transcript", n_iter);

      // Clear current line transcript for next line
      currentLineTranscript.Truncate();
      lastSegmentText.Truncate();

      // Keep part of audio for next iteration (similar to cubeb-transcript line
      // 947)
      pcmf32_old =
          std::vector<float>(pcmf32.end() - n_samples_keep, pcmf32.end());

      // Update prompt tokens if context carryover is enabled
      if (mParams.mUseContextCarryover) {
        mPromptTokens.clear();
        for (int i = 0; i < n_segments; ++i) {
          const int token_count = mLib->whisper_full_n_tokens(mWhisperCtx, i);
          for (int j = 0; j < token_count; ++j) {
            mPromptTokens.push_back(
                mLib->whisper_full_get_token_id(mWhisperCtx, i, j));
          }
        }
        if (mPromptTokens.size() > (size_t)mParams.mMaxContextTokens) {
          mPromptTokens.erase(
              mPromptTokens.begin(),
              mPromptTokens.begin() +
                  (mPromptTokens.size() - mParams.mMaxContextTokens));
        }
      }
    }

    lastRecognitionTime = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Send final transcript on shutdown if we have any pending text
  if (!currentLineTranscript.IsEmpty()) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "SpeechRecognitionParent::SendFinalOnExit",
        [self = RefPtr{this}, payload = currentLineTranscript]() {
          if (self->CanSend()) {
            LOGD("Sending final transcript on shutdown: '{}'", payload.get());
            Unused << self->SendOnRecognitionResult(payload, true);
          }
        }));
  }
  LOGD("Recognition loop terminated");
}

}  // namespace mozilla::ipc

#undef LOGV
#undef LOGD
#undef LOGE
