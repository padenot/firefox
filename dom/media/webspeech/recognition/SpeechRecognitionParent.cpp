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
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/FileDescriptorUtils.h"
#include "mozilla/ipc/HWInferenceChild.h"
#include "mozilla/ipc/PHWInference.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/llama/LlamaRuntimeLinker.h"
#include "nsDebug.h"
#include "nsGkAtoms.h"
#include "nsNetUtil.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "prio.h"
#include "private/pprio.h"

#ifdef XP_WIN
#  include <fcntl.h>
#endif

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
  return nsFmtCString("{}/{}/{}", mModelName.get(), mFileName.get(),
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

  hwInferenceChild
      ->SendIsModelAvailable(modelIdentifier.mModelName,
                             modelIdentifier.mRevision,
                             modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}, aResolver](bool aAvailable) mutable {
            LOGD("Sending response back to content process: available={}",
                 aAvailable ? "true" : "false");
            aResolver(aAvailable);
          },
          [self = RefPtr{this},
           aResolver](ResponseRejectReason aReason) mutable {
            LOGE("{} IPC call to main process failed: {}", __func__,
                 static_cast<int>(aReason));
            aResolver(false);
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
      ->SendInstallModel("speech-recognition"_ns, modelIdentifier.mModelName,
                         modelIdentifier.mRevision, modelIdentifier.mFileName)
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
      mWhisperCtx(nullptr),
      // We expect that in some less powerful computer that aren't doing hw
      // accelerated recognition, having a very long queue can smooth things
      // out.
      mAudioQueue(WHISPER_SAMPLE_RATE * 30),
      mShouldContinueProcessing(false),
      mParams(),
      mProcessedAudioPos(0) {
  // MOZ_DUMP_AUDIO=1 MOZ_DISABLE_UTILITY_SANDBOX=1 to activate this
  // It will contain the (repeating segments of audio), precisely that has been
  // sent to whisper.cpp
  static const int MONO = 1;
  mWhisperAudioDumper.Open("SpeechRecognition-Whisper-Input", MONO,
                           WHISPER_SAMPLE_RATE);

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

void SpeechRecognitionParent::RetrieveModel() {
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
    LOGE("{} No HWInferenceChild available for model retrieval", __func__);
    ResolveOrRejectInitOnIPCThread(false);
    return;
  }

  MutexAutoLock lock(mLock);
  ModelIdentifier modelIdentifier =
      LanguagesToModelIdentifier(nsTArray{mLanguage});

  LOGD("{} Requesting model: model={}", __func__,
       modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendGetModelFile(nsCString("speech-recognition"),
                         modelIdentifier.mModelName, modelIdentifier.mRevision,
                         modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}](
              const mozilla::ipc::GetModelFileResult& aResult) mutable {
            if (aResult.type() ==
                mozilla::ipc::GetModelFileResult::TGetModelError) {
              LOGE("{} GetModelError with nsresult={:x}", __func__,
                   static_cast<uint32_t>(
                       aResult.get_GetModelError().errorCode()));
              self->ResolveOrRejectInitOnIPCThread(false);
              return;
            }

            // Convert FileDescriptor to FILE* using the helper function
            mozilla::ipc::FileDescriptor fd =
                aResult.get_GetModelFileSuccess().fd();

            // Store the file handle on the main thread
            {
              MutexAutoLock lock(self->mLock);
              FILE* file = FileDescriptorToFILE(fd, "rb");
              if (!file) {
                LOGE("{} Failed to convert FileDescriptor to FILE*", __func__);
                self->ResolveOrRejectInitOnIPCThread(false);
                return;
              }
              self->mModelFile.reset(file);
            }

            // Signal the recognition thread that the model is ready
            LOGD("Model file ready, starting recognition thread");
            nsresult rv = NS_NewNamedThread(
                "Whisper", getter_AddRefs(self->mRecognitionThread),
                NS_NewRunnableFunction("Initialize whisper context", [self]() {
                  self->InitializeWhisperContext();
                }));
            if (NS_FAILED(rv)) {
              LOGE("Failed to create recognition thread: {:x}",
                   static_cast<uint32_t>(rv));
              self->mShouldContinueProcessing.store(false);
              self->ResolveOrRejectInitOnIPCThread(false);
            }
          },
          [self = RefPtr{this}](
              mozilla::ipc::ResponseRejectReason aReason) mutable {
            LOGE("{} Promise rejected with reason {}", __func__,
                 static_cast<int>(aReason));
            self->ResolveOrRejectInitOnIPCThread(false);
          });
}

void SpeechRecognitionParent::InitializeWhisperContext() {
  // This runs on the recognition thread
  MOZ_ASSERT(!NS_IsMainThread());

  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();
  if (!lib) {
    LOGE("{} Failed to get runtime linker", __func__);
    ResolveOrRejectInitOnIPCThread(false);
    return;
  }

  struct whisper_context_params cparams = lib->whisper_context_default_params();
#ifdef XP_MACOSX
  cparams.use_gpu = true;
#else
  cparams.use_gpu = false;
#endif

  FILE* modelFile = nullptr;
  {
    MutexAutoLock lock(mLock);
    modelFile = mModelFile.get();
  }

  mWhisperCtx =
      lib->whisper_init_from_file_handle_with_params(modelFile, cparams);
  if (!mWhisperCtx) {
    LOGE("{} whisper_init_from_file_handle_with_params failed", __func__);
    ResolveOrRejectInitOnIPCThread(false);
    return;
  }

  ResolveOrRejectInitOnIPCThread(true);
  LOGD("Whisper context ready, starting main recognition loop");

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
}

void SpeechRecognitionParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("{} ActorDestroy called", __func__);

  MutexAutoLock lock(mLock);
  if (mModelFile) {
    mModelFile = nullptr;
  }
  // Signal the recognition thread to stop before shutting it down
  mShouldContinueProcessing.store(false);

  if (mRecognitionThread) {
    mRecognitionThread->Shutdown();
    mRecognitionThread = nullptr;
  }

  if (mWhisperCtx) {
    mozilla::llama::LlamaLibWrapper* lib =
        mozilla::llama::LlamaRuntimeLinker::Get();
    if (lib) {
      lib->whisper_free(mWhisperCtx);
    }
    mWhisperCtx = nullptr;
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

  {
    MutexAutoLock lock(mLock);
    mLanguage = aLanguage;
    mPhrases = aPhrases.Clone();
  }

  // What follows is a long chain of asynchronous steps within and outside of
  // this process. Stash the resolver here to properly call it when it's time.
  mInitResolver = aResolver;

  // We perform the part of the initialization on the same thread we'll later
  // use for the main recognition loop.
  mShouldContinueProcessing.store(true);

  RetrieveModel();

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvProcessAudioData(
    nsTArray<float>&& aAudioData) {
  LOGV("{} {} samples", __func__, aAudioData.Length());

  if (!mAudioQueue.Enqueue(aAudioData.Elements(),
                           static_cast<int>(aAudioData.Length()))) {
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

  mShouldContinueProcessing.store(false);

  LOGD("Stopping speech recognition session and cleaning up resources");
  return IPC_OK();
}

whisper_full_params SpeechRecognitionParent::GetWhisperParams() {
  enum whisper_sampling_strategy strat =
      static_cast<enum whisper_sampling_strategy>(
          (mParams.mBeamSize > 1) ? WHISPER_SAMPLING_BEAM_SEARCH
                                  : WHISPER_SAMPLING_GREEDY);

  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();
  whisper_full_params wparams = lib->whisper_full_default_params(strat);
  wparams.print_progress = false;
  wparams.print_special = false;
  wparams.print_realtime = false;
  wparams.print_timestamps = true;
  wparams.translate = false;
  wparams.single_segment = mParams.mSingleSegment;
  wparams.max_tokens =
      mParams.mMaxTokensPerSegment;  // 0 = unlimited (recommended)
  wparams.n_threads = mParams.mNumThreads;
  wparams.audio_ctx = mParams.mAudioContextSize;

  wparams.temperature = mParams.mTemperature;
  wparams.temperature_inc = mParams.mTemperatureInc;
  wparams.beam_search.beam_size = mParams.mBeamSize;
  wparams.greedy.best_of = mParams.mBestOf;
  wparams.entropy_thold = mParams.mEntropyThreshold;
  wparams.logprob_thold = mParams.mLogProbThreshold;
  wparams.no_speech_thold = mParams.mNoSpeechThreshold;

  if (mParams.mUseContextCarryover) {
    wparams.no_context = false;
  } else {
    wparams.prompt_tokens = nullptr;
    wparams.prompt_n_tokens = 0;
    wparams.no_context = true;
  }

  return wparams;
}

void SpeechRecognitionParent::SignalError(const nsCString& aErrorMessage) {
  LOGE("Error: {}", aErrorMessage.get());
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "SpeechRecognitionParent::SignalError",
      [self = RefPtr{this}, aErrorMessage]() {
        if (!self->SendOnRecognitionError(aErrorMessage)) {
          LOGE("Counldn't send OnRecognitionError for {}", aErrorMessage);
        }
      }));
}

void SpeechRecognitionParent::ProcessAudioOnBackgroundThread() {
  LOGD("{} Starting continuous recognition loop", __func__);

  // This function doesn't use the usual Gecko data structures and idioms,
  // because it can be copied back and forth into a standalone C++ program that
  // can be used for very fast iteration, that might well become vendored
  // in m-c in the future. I anticipate that some more tuning and more advanced
  // audio input preparation and token output massaging is needed to improve
  // the overall quality of the recognition, and the latency.

  // Amount of new audio in an inference step. Typically a small number of
  // seconds.
  const size_t samplePerStep =
      size_t((1e-3 * mParams.mStepMs) * WHISPER_SAMPLE_RATE);
  // Total amount of audio in an inference step, typically 10 to 30 seconds
  // (which is the maximum whisper supports, and also the audio duration it has
  // been trained as).
  const size_t stepSampleCount =
      size_t(1e-3 * mParams.mAudioLengthMs * WHISPER_SAMPLE_RATE);
  // Amount of sample we keep from a step to the next, to improved recognition
  // in case we've split a word in two.
  const size_t keptSamples =
      std::min(size_t(1e-3 * mParams.mKeepAudioMs * WHISPER_SAMPLE_RATE),
               stepSampleCount);

  // Calculate number of iterations before we decide that a recognition is
  // "complete", marking the result as final, and we start over with mostly
  // fresh audio.
  const int iterationPerLine =
      std::max(1, mParams.mAudioLengthMs / mParams.mStepMs - 1);
  int iterationCount = 0;

  std::vector<float> pcmf32(stepSampleCount, 0.0f);
  std::vector<float> oldAudio;
  std::vector<float> newAudio;

  // Current line's accumulated transcript
  nsCString currentLineTranscript;
  // Last segment text to avoid duplicates
  nsCString lastSegmentText;

  // Tokens from previous segment for context, only used when prompt carryover
  // has been enabled.
  std::vector<int32_t> promptTokens;

  // Prompt. This comes from the "phrases" member of the Web Speech API.
  nsCString language;
  nsCString prompt;
  {
    MutexAutoLock lock(mLock);
    language = mLanguage;
    for (const auto& phrase : mPhrases) {
      prompt.Append(NS_ConvertUTF16toUTF8(phrase));
      prompt.AppendLiteral(". ");
    }
  }

  auto lastRecognitionTime = std::chrono::steady_clock::now();

  while (mShouldContinueProcessing.load()) {
    size_t available = mAudioQueue.AvailableRead();
    if (available < samplePerStep) {
      float ms_to_sleep = 1000.f *
                          static_cast<float>(samplePerStep - available) /
                          WHISPER_SAMPLE_RATE;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int>(ms_to_sleep)));
      continue;
    }

    // Dequeue new audio from our lock-free ringbuffer into a linear buffer
    newAudio.resize(samplePerStep);
    size_t dequeued =
        mAudioQueue.Dequeue(newAudio.data(), AssertedCast<int>(samplePerStep));
    if (dequeued < AssertedCast<size_t>(samplePerStep)) {
      newAudio.resize(dequeued);
    }

    mProcessedAudioPos += dequeued;

    // Check timing. We might want to go off a clock synthesized from the SPSC
    // queue here instead.
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

    // Take up to keepMS audio from previous iteration
    const size_t neededSamples = std::min(
        oldAudio.size(),
        std::max(0ul, keptSamples + stepSampleCount - newAudio.size()));

    pcmf32.resize(newAudio.size() + neededSamples);

    // for (int i = 0; i < neededSamples; i++) {
    //   pcmf32[i] = oldAudio[oldAudio.size() - neededSamples + i];
    // }

    // Copy old samples that we're keeping from previous step
    size_t offset = oldAudio.size() - neededSamples;
    memcpy(pcmf32.data(), oldAudio.data() + offset,
           neededSamples * sizeof(float));
    // Followed by the new samples
    memcpy(pcmf32.data() + neededSamples, newAudio.data(),
           newAudio.size() * sizeof(float));

    oldAudio = pcmf32;

    // Dump audio for debugging
    mWhisperAudioDumper.Write(pcmf32.data(), pcmf32.size());

    whisper_full_params wparams = GetWhisperParams();
    wparams.language = language.get();
    wparams.initial_prompt = prompt.IsEmpty() ? nullptr : prompt.get();
    wparams.prompt_tokens =
        promptTokens.empty() ? nullptr : promptTokens.data();
    wparams.prompt_n_tokens = static_cast<int>(promptTokens.size());

    mozilla::llama::LlamaLibWrapper* lib =
        mozilla::llama::LlamaRuntimeLinker::Get();
    if (lib->whisper_full(mWhisperCtx, wparams, pcmf32.data(),
                          static_cast<int>(pcmf32.size()))) {
      SignalError("whisper_full failed"_ns);
      return;
    }

    const int nSegments = lib->whisper_full_n_segments(mWhisperCtx);
    bool appendedAnything = false;

    for (int i = 0; i < nSegments; ++i) {
      const char* text = lib->whisper_full_get_segment_text(mWhisperCtx, i);
      if (!text || !text[0]) {
        continue;
      }
      nsCString segmentText(text);
      segmentText.Trim(" \t\n\r");

      // Skip empty or duplicate segments, this can happen with some whisper
      // models that hallucinate repetitions.
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
    iterationCount++;

    bool isNewLine = (iterationCount % iterationPerLine) == 0;

    // Send results if we have new content
    if (appendedAnything && !currentLineTranscript.IsEmpty()) {
      // Send as FINAL if this is the end of a line, INTERIM otherwise
      bool isFinal = isNewLine;

      NS_DispatchToMainThread(NS_NewRunnableFunction(
          "SpeechRecognitionParent::SendResult",
          [self = RefPtr{this}, payload = currentLineTranscript, isFinal]() {
            LOGV("Sending result: '{}' (final={})", payload.get(), isFinal);
            if (!self->SendOnRecognitionResult(payload, isFinal)) {
              self->SignalError(
                  nsFmtCString("Couldn't send recognition result {}, final={}",
                               payload.get(), isFinal));
            }
          }));
    }

    // If new line detected, clear transcript for next line
    if (isNewLine) {
      LOGD("New line detected at iteration {}, clearing transcript",
           iterationCount);

      currentLineTranscript.Truncate();
      lastSegmentText.Truncate();

      // Clear audio, but keep a little bit of it to improve recognition, if a
      // word was cut in two. This will be improved by using a more advanced
      // audio processing algorithm, such as splitting during low energy
      // periods.
      oldAudio = std::vector<float>(pcmf32.end() - keptSamples, pcmf32.end());

      // Update prompt tokens if context carryover is enabled
      if (mParams.mUseContextCarryover) {
        promptTokens.clear();
        for (int i = 0; i < nSegments; ++i) {
          const int token_count = lib->whisper_full_n_tokens(mWhisperCtx, i);
          for (int j = 0; j < token_count; ++j) {
            promptTokens.push_back(
                lib->whisper_full_get_token_id(mWhisperCtx, i, j));
          }
        }
        if (promptTokens.size() > (size_t)mParams.mMaxContextTokens) {
          promptTokens.erase(
              promptTokens.begin(),
              promptTokens.begin() +
                  (promptTokens.size() - mParams.mMaxContextTokens));
        }
      }
    }

    lastRecognitionTime = now;
  }

  // Send final transcript on shutdown if we have any pending text
  if (!currentLineTranscript.IsEmpty()) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "SpeechRecognitionParent::SendFinalOnExit",
        [self = RefPtr{this}, payload = currentLineTranscript]() {
          if (self->CanSend()) {
            LOGD("Sending final transcript on shutdown: '{}'", payload.get());
            (void)self->SendOnRecognitionResult(payload, true);
          }
        }));
  }
  LOGD("Recognition loop exiting");
}

}  // namespace mozilla::ipc

#undef LOGV
#undef LOGD
#undef LOGE
