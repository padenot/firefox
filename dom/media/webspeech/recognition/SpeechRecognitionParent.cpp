/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8  et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionParent.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "mozIRemoteLazyInputStream.h"
#include "mozilla/Logging.h"
#include "mozilla/Mutex.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/hwinference/HWInferenceChild.h"
#include "mozilla/ipc/FileDescriptorUtils.h"
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

namespace mozilla {
void ParakeetContextDeleter::operator()(parakeet_context* ctx) {
  if (ctx) {
    mozilla::llama::LlamaLibWrapper* lib =
        mozilla::llama::LlamaRuntimeLinker::Get();
    if (lib) {
      lib->parakeet_free(ctx);
    }
  }
}
}  // namespace mozilla

namespace mozilla {

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
  if (!mParams.mStreamingBackend) {
    // Legacy whisper.cpp-fork offline model.
    return {"cstr/parakeet-tdt-0.6b-v3-GGUF"_ns,
            "parakeet-tdt-0.6b-v3-q4_0.gguf"_ns, "main"_ns};
  }
  // mudler/parakeet.cpp cache-aware streaming GGUFs, hosted on the Mozilla
  // model hub under asr-test/parakeet. English uses the small EOU model;
  // everything else uses the multilingual nemotron model.
  const bool english =
      aLanguages.IsEmpty() || StringBeginsWith(aLanguages[0], "en"_ns);
  if (english) {
    return {"asr-test/parakeet"_ns, "realtime_eou_120m-v1-q5_k.gguf"_ns,
            "main"_ns};
  }
  return {"asr-test/parakeet"_ns, "nemotron-3.5-asr-streaming-0.6b-q5_k.gguf"_ns,
          "main"_ns};
}

nsCString SpeechRecognitionParent::ModelIdentifier::ToString() const {
  return nsFmtCString("{}/{}/{}", mModelName.get(), mFileName.get(),
                      mRevision.get());
}

void SpeechRecognitionParent::ResolveOrRejectInitOnIPCThread(
    InitResolver&& aResolver, bool aSuccess) {
  if (GetActorEventTarget()->IsOnCurrentThread()) {
    LOGV("Resolving init on same thread {}", aSuccess);
    aResolver(aSuccess);
  } else {
    LOGV("Resolving init accross thread {}", aSuccess);
    GetActorEventTarget()->Dispatch(NS_NewRunnableFunction(
        "Speech recognition init runnable",
        [resolver = std::move(aResolver), aSuccess]() {
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

  mozilla::hwinference::HWInferenceChild* hwInferenceChild =
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
      ->SendIsModelAvailable("parakeet-gguf"_ns, modelIdentifier.mModelName,
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

  mozilla::hwinference::HWInferenceChild* hwInferenceChild =
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
      // We expect that in some less powerful computer that aren't doing hw
      // accelerated recognition, having a very long queue can smooth things
      // out.
      mAudioQueue(PARAKEET_SAMPLE_RATE * 30),
      mParams(),
      mShouldContinueProcessing(false),
      mProcessedAudioPos(0) {
  // MOZ_DUMP_AUDIO=1 MOZ_DISABLE_UTILITY_SANDBOX=1 to activate this
  // It will contain the (repeating segments of audio), precisely that has been
  // sent to whisper.cpp
  const int MONO = 1;
  mWhisperAudioDumper.Open("SpeechRecognition-Whisper-Input", MONO,
                           PARAKEET_SAMPLE_RATE);

  // Load tunable parameters from preferences (can be overridden via
  // about:config)
  LoadPreferences();
}

void SpeechRecognitionParent::LoadPreferences() {
  // Timing parameters
  mParams.mRecognitionIntervalMs =
      Preferences::GetInt("media.webspeech.recognition.interval_ms", 500);
  // Length of the sliding context window fed to the encoder each step.
  mParams.mAudioLengthMs =
      Preferences::GetInt("media.webspeech.recognition.audio_length_ms", 8000);
  mParams.mKeepAudioMs =
      Preferences::GetInt("media.webspeech.recognition.keep_audio_ms", 200);
  // How much new audio is consumed per inference step: this bounds latency.
  mParams.mStepMs =
      Preferences::GetInt("media.webspeech.recognition.step_ms", 1000);

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

  // Backend selection: cache-aware streaming (mudler/parakeet.cpp) vs the
  // legacy whisper.cpp-fork sliding-window path.
  mParams.mStreamingBackend = Preferences::GetBool(
      "media.webspeech.recognition.streaming_backend", true);

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

void SpeechRecognitionParent::RetrieveModel(InitResolver&& aResolver) {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} ERROR: No UtilityProcessChild available", __func__);
    ResolveOrRejectInitOnIPCThread(std::move(aResolver), false);
    return;
  }
  mozilla::hwinference::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available for model retrieval", __func__);
    ResolveOrRejectInitOnIPCThread(std::move(aResolver), false);
    return;
  }

  ModelIdentifier modelIdentifier;
  {
    MutexAutoLock lock(mLock);
    modelIdentifier = LanguagesToModelIdentifier(nsTArray{mLanguage});
  }

  LOGD("{} Requesting model: model={}", __func__,
       modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendGetModelFile("parakeet-gguf"_ns, "speech-recognition"_ns,
                         modelIdentifier.mModelName, modelIdentifier.mRevision,
                         modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}, resolver = aResolver](
              const mozilla::hwinference::GetModelFileResult& aResult) mutable {
            if (aResult.type() ==
                mozilla::hwinference::GetModelFileResult::TGetModelError) {
              LOGE("{} GetModelError with nsresult={:x}", __func__,
                   static_cast<uint32_t>(
                       aResult.get_GetModelError().errorCode()));
              self->ResolveOrRejectInitOnIPCThread(std::move(resolver), false);
              return;
            }

            // Convert FileDescriptor to FILE* using the helper function
            mozilla::ipc::FileDescriptor fd =
                aResult.get_GetModelFileSuccess().fd();

            FILE* file = FileDescriptorToFILE(fd, "rb");
            if (!file) {
              LOGE("{} Failed to convert FileDescriptor to FILE*", __func__);
              self->ResolveOrRejectInitOnIPCThread(std::move(resolver), false);
              return;
            }
            // Store the file handle on the main thread
            {
              MutexAutoLock lock(self->mLock);
              self->mModelFile.reset(file);
            }

            // Signal the recognition thread that the model is ready
            LOGD("Model file ready, starting recognition thread");
            nsresult rv = NS_NewNamedThread(
                "Parakeet", getter_AddRefs(self->mRecognitionThread),
                NS_NewRunnableFunction(
                    "Initialize parakeet context",
                    [self, resolver = std::move(resolver)]() mutable {
                      self->InitializeParakeetContext(std::move(resolver));
                    }));
            if (NS_FAILED(rv)) {
              LOGE("Failed to create recognition thread: {:x}",
                   static_cast<uint32_t>(rv));
              self->ResolveOrRejectInitOnIPCThread(std::move(resolver), false);
            }
          },
          [self = RefPtr{this}, resolver = aResolver](
              mozilla::ipc::ResponseRejectReason aReason) mutable {
            LOGE("{} Promise rejected with reason {}", __func__,
                 static_cast<int>(aReason));
            self->ResolveOrRejectInitOnIPCThread(std::move(resolver), false);
          });
}

void SpeechRecognitionParent::InitializeParakeetContext(
    InitResolver&& aResolver) {
  // This runs on the recognition thread
  MOZ_ASSERT(!NS_IsMainThread());

  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();
  if (!lib) {
    LOGE("{} Failed to get runtime linker", __func__);
    ResolveOrRejectInitOnIPCThread(std::move(aResolver), false);
    return;
  }

  struct parakeet_context_params cparams =
      lib->parakeet_context_default_params();
#ifdef XP_MACOSX
  cparams.use_gpu = true;
#else
  cparams.use_gpu = false;
#endif

  FILE* modelFile = nullptr;
  nsCString language;
  {
    MutexAutoLock lock(mLock);
    modelFile = mModelFile.get();
    language = mLanguage;
  }

  if (mParams.mStreamingBackend) {
    // Cache-aware streaming backend (mudler/parakeet.cpp): load the model from
    // the fd, then open a streaming session for the recognition language.
    mCapiCtx = lib->parakeet_capi_load_fd(fileno(modelFile));
    if (!mCapiCtx) {
      LOGE("{} parakeet_capi_load_fd failed", __func__);
      ResolveOrRejectInitOnIPCThread(std::move(aResolver), false);
      return;
    }
    const char* langArg = language.IsEmpty() ? nullptr : language.get();
    mCapiStream = lib->parakeet_capi_stream_begin_lang(mCapiCtx, langArg);
    if (!mCapiStream && langArg) {
      // The multilingual model rejects languages outside its dictionary; rather
      // than fail the session, fall back to auto-detection.
      LOGD("stream_begin_lang('{}') failed; falling back to auto-detection",
           langArg);
      mCapiStream = lib->parakeet_capi_stream_begin_lang(mCapiCtx, "auto");
    }
    if (!mCapiStream) {
      LOGE("{} parakeet_capi_stream_begin_lang failed", __func__);
      ResolveOrRejectInitOnIPCThread(std::move(aResolver), false);
      return;
    }
    mShouldContinueProcessing.store(true);
    ResolveOrRejectInitOnIPCThread(std::move(aResolver), true);
    LOGD("Parakeet streaming session ready, starting streaming loop");
    mRecognitionThread->Dispatch(NS_NewRunnableFunction(
        "Parakeet streaming loop",
        [self = RefPtr{this}] { self->ProcessAudioStreaming(); }));
    return;
  }

  mParakeetCtx.reset(
      lib->parakeet_init_from_fd_with_params(fileno(modelFile), cparams));
  if (!mParakeetCtx) {
    LOGE("{} parakeet_init_from_fd_with_params failed", __func__);
    ResolveOrRejectInitOnIPCThread(std::move(aResolver), false);
    return;
  }

  mShouldContinueProcessing.store(true);
  ResolveOrRejectInitOnIPCThread(std::move(aResolver), true);
  LOGD("Parakeet context ready, starting main recognition loop");

  mRecognitionThread->Dispatch(NS_NewRunnableFunction(
      "Parakeet recognition loop",
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

  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session in ActorDestroy");
      sActiveSession = nullptr;
    }
  }

  mShouldContinueProcessing.store(false);

  MutexAutoLock lock(mLock);
  if (mModelFile) {
    mModelFile = nullptr;
  }

  if (mRecognitionThread) {
    mRecognitionThread->Shutdown();
    mRecognitionThread = nullptr;
  }

  // The recognition thread is joined above, so the streaming handles are no
  // longer in use and can be freed.
  if (mCapiStream || mCapiCtx) {
    mozilla::llama::LlamaLibWrapper* lib =
        mozilla::llama::LlamaRuntimeLinker::Get();
    if (lib) {
      if (mCapiStream) {
        lib->parakeet_capi_stream_free(mCapiStream);
      }
      if (mCapiCtx) {
        lib->parakeet_capi_free(mCapiCtx);
      }
    }
    mCapiStream = nullptr;
    mCapiCtx = nullptr;
  }

  mParakeetCtx.reset();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInit(
    const nsCString& aEngineId, const nsCString& aLanguage,
    const nsTArray<nsString>& aPhrases, InitResolver&& aResolver) {
  LOGD("{} engineId='{}' language='{}'", __func__, aEngineId.get(),
       aLanguage.get());

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

  RetrieveModel(std::move(aResolver));

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

parakeet_full_params SpeechRecognitionParent::GetParakeetParams() {
  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();
  parakeet_full_params params =
      lib->parakeet_full_default_params(PARAKEET_SAMPLING_GREEDY);
  params.n_threads = mParams.mNumThreads;
  params.audio_ctx = mParams.mAudioContextSize;
  params.no_context = !mParams.mUseContextCarryover;
  return params;
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
  //
  // Low-latency streaming via a sliding context window with LocalAgreement-2.
  // Every mStepMs of new audio we re-run parakeet_full() over a trailing window
  // of up to mAudioLengthMs. The conformer encoder runs over the whole window,
  // so each inference has enough acoustic left-context to recognise the most
  // recent words accurately even though we advance by a small step (which is
  // what bounds the latency). We then only *commit* the longest prefix of
  // tokens that two consecutive inferences agree on, discarding the unstable
  // tail near the right edge of the window. Token positions come from
  // parakeet_token_data::t0/t1, expressed in mel frames of PARAKEET_HOP_LENGTH
  // samples each, which lets us track how far into the absolute audio stream we
  // have committed across windows. The model is run with no_context=true: it is
  // the cross-window agreement, not decoder state, that stabilises the output.
  //
  // The "true" low-latency path is cache-aware streaming (retain the encoder
  // cache and decode incrementally), which would avoid re-encoding the window
  // every step, but the vendored parakeet API does not expose it yet.

  // Amount of new audio consumed per inference step. Bounds the latency.
  const size_t advanceSamples = std::max<size_t>(
      1, size_t(1e-3 * mParams.mStepMs * PARAKEET_SAMPLE_RATE));
  // Maximum length of the sliding context window fed to the encoder.
  const size_t windowSamples =
      std::max(advanceSamples,
               size_t(1e-3 * mParams.mAudioLengthMs * PARAKEET_SAMPLE_RATE));
  // Emit a final result once this much speech has been committed since the last
  // one, consolidating the interim results the consumer has already seen.
  const size_t samplesPerFinal = windowSamples;

  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();

  // Renders a single token to text. parakeet_token_to_text() turns the raw
  // sentencepiece piece into text, encoding inter-word spacing as a leading
  // space on word-start pieces (suppressed when aIsFirst).
  auto renderToken = [&](parakeet_token aId, bool aIsFirst) -> nsCString {
    const char* piece = lib->parakeet_token_to_str(mParakeetCtx.get(), aId);
    if (!piece) {
      return nsCString();
    }
    char buf[256];
    int len = lib->parakeet_token_to_text(piece, aIsFirst, buf, sizeof(buf));
    if (len <= 0) {
      return nsCString();
    }
    return nsCString(buf);
  };

  auto dispatchResult = [self = RefPtr{this}](const nsCString& aPayload,
                                              bool aIsFinal) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "SpeechRecognitionParent::SendResult", [self, aPayload, aIsFinal]() {
          LOGV("Sending result: '{}' (final={})", aPayload.get(), aIsFinal);
          // The legacy sliding-window backend has no per-word confidence.
          if (self->CanSend() &&
              !self->SendOnRecognitionResult(aPayload, aIsFinal, 1.0f)) {
            self->SignalError(
                nsFmtCString("Couldn't send recognition result {}, final={}",
                             aPayload.get(), aIsFinal));
          }
        }));
  };

  struct Token {
    parakeet_token mId;
    int64_t mStartSample;
    int64_t mEndSample;
  };

  // Rolling context window, and the absolute sample index of its first sample.
  nsTArray<float> window;
  int64_t windowStartSample = 0;
  // Hypothesis from the previous inference, for the LocalAgreement comparison.
  nsTArray<Token> prevTokens;
  // Absolute sample position up to which tokens have been committed.
  int64_t committedSample = 0;
  // Text committed but not yet flushed as a final result.
  nsCString pendingFinal;
  size_t committedSinceFinal = 0;
  // Whether anything has been committed in the current final segment (drives
  // leading-space handling for the first rendered token).
  bool anyCommitted = false;

  // Commit a single token: append its text and advance the commit position.
  auto commitToken = [&](const Token& aTok) {
    pendingFinal.Append(renderToken(aTok.mId, !anyCommitted));
    anyCommitted = true;
    committedSample = aTok.mEndSample;
    committedSinceFinal +=
        size_t(std::max<int64_t>(0, aTok.mEndSample - aTok.mStartSample));
  };
  // Index of the first not-yet-committed token in a hypothesis.
  auto firstUncommitted = [&](const nsTArray<Token>& aToks) -> size_t {
    size_t i = 0;
    while (i < aToks.Length() && aToks[i].mStartSample < committedSample) {
      ++i;
    }
    return i;
  };

  nsTArray<float> incoming;

  while (mShouldContinueProcessing.load()) {
    size_t available = mAudioQueue.AvailableRead();
    if (available < advanceSamples) {
      float msToSleep = 1000.f *
                        static_cast<float>(advanceSamples - available) /
                        PARAKEET_SAMPLE_RATE;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int>(msToSleep)));
      continue;
    }

    incoming.SetLength(advanceSamples);
    size_t dequeued = mAudioQueue.Dequeue(incoming.Elements(),
                                          AssertedCast<int>(advanceSamples));
    incoming.SetLength(dequeued);
    mProcessedAudioPos += dequeued;

    window.AppendElements(incoming);
    // Slide the window, advancing the absolute start of its first sample.
    if (window.Length() > windowSamples) {
      size_t excess = window.Length() - windowSamples;
      window.RemoveElementsAt(0, excess);
      windowStartSample += excess;
    }

    // Force-commit tokens from the previous hypothesis that have just slid out
    // of the window: they are too old to ever reach agreement now and would
    // otherwise be silently dropped. By the time a token reaches the left edge
    // it has had the whole window of context, so it is stable.
    for (size_t k = firstUncommitted(prevTokens);
         k < prevTokens.Length() &&
         prevTokens[k].mStartSample < windowStartSample;
         ++k) {
      commitToken(prevTokens[k]);
    }

    // Dump audio for debugging
    mWhisperAudioDumper.Write(window.Elements(), window.Length());

    parakeet_full_params wparams = GetParakeetParams();
    // Each window is transcribed from scratch; agreement across windows, not
    // decoder state carryover, is what makes the committed output stable.
    wparams.no_context = true;
    if (lib->parakeet_full(mParakeetCtx.get(), wparams, window.Elements(),
                           static_cast<int>(window.Length()))) {
      SignalError("parakeet_full failed"_ns);
      return;
    }

    // Flatten the hypothesis into absolute-positioned tokens.
    nsTArray<Token> tokens;
    const int nSegments = lib->parakeet_full_n_segments(mParakeetCtx.get());
    for (int s = 0; s < nSegments; ++s) {
      const int nTokens = lib->parakeet_full_n_tokens(mParakeetCtx.get(), s);
      for (int t = 0; t < nTokens; ++t) {
        parakeet_token_data data =
            lib->parakeet_full_get_token_data(mParakeetCtx.get(), s, t);
        tokens.AppendElement(Token{
            data.id, windowStartSample + int64_t(data.t0) * PARAKEET_HOP_LENGTH,
            windowStartSample + int64_t(data.t1) * PARAKEET_HOP_LENGTH});
      }
    }

    const size_t newStart = firstUncommitted(tokens);
    const size_t prevStart = firstUncommitted(prevTokens);

    // LocalAgreement-2: commit the longest prefix of the uncommitted tokens
    // that is identical between this hypothesis and the previous one.
    size_t agreed = 0;
    while (newStart + agreed < tokens.Length() &&
           prevStart + agreed < prevTokens.Length() &&
           tokens[newStart + agreed].mId ==
               prevTokens[prevStart + agreed].mId) {
      commitToken(tokens[newStart + agreed]);
      ++agreed;
    }

    prevTokens = std::move(tokens);

    // Interim transcript: committed text plus the still-unstable tail of the
    // current hypothesis, so the consumer sees words as soon as they decode.
    nsCString interim(pendingFinal);
    bool first = !anyCommitted;
    for (size_t j = newStart + agreed; j < prevTokens.Length(); ++j) {
      interim.Append(renderToken(prevTokens[j].mId, first));
      first = false;
    }
    interim.Trim(" \t\n\r");
    if (!interim.IsEmpty()) {
      dispatchResult(interim, /* isFinal */ false);
    }

    if (committedSinceFinal >= samplesPerFinal && !pendingFinal.IsEmpty()) {
      nsCString payload(pendingFinal);
      payload.Trim(" \t\n\r");
      dispatchResult(payload, /* isFinal */ true);
      pendingFinal.Truncate();
      committedSinceFinal = 0;
      anyCommitted = false;
    }
  }

  // Flush whatever has been committed since the last final, as the final.
  if (!pendingFinal.IsEmpty()) {
    nsCString payload(pendingFinal);
    payload.Trim(" \t\n\r");
    LOGD("Sending final transcript on shutdown: '{}'", payload.get());
    dispatchResult(payload, /* isFinal */ true);
  }
  LOGD("Recognition loop exiting");
}

void SpeechRecognitionParent::ProcessAudioStreaming() {
  LOGD("{} Starting cache-aware streaming loop", __func__);

  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();

  // The model keeps its own encoder/decoder caches across feeds, so we just
  // hand it new audio as it arrives. parakeet_capi_stream_feed returns the
  // text newly committed by this feed (a cache-aware transducer never revises
  // past output) plus, via the out-param, an EOU/EOB bitmask. Each committed
  // delta is emitted as a final result at streaming latency.
  // Feed promptly: the content process already streams audio in small blocks,
  // and the model buffers internally until its chunk fills, so we just forward
  // whatever has arrived. A small floor avoids spinning on sub-block wakeups.
  const size_t minFeed = size_t(0.01 * PARAKEET_SAMPLE_RATE);  // 10 ms
  const size_t maxFeed = size_t(PARAKEET_SAMPLE_RATE);         // 1 s

  // Strip inline <...> markers (e.g. nemotron <en-US> language tags); the
  // <EOU>/<EOB> markers are already surfaced via the event bitmask.
  auto stripTags = [](nsCString& aText) {
    int32_t open;
    while ((open = aText.FindChar('<')) != kNotFound) {
      int32_t close = aText.FindChar('>', open);
      if (close == kNotFound) {
        break;
      }
      aText.Cut(open, close - open + 1);
    }
  };

  auto emit = [self = RefPtr{this}](const nsCString& aText, bool aFinal,
                                    float aConfidence) {
    if (aText.IsEmpty()) {
      return;
    }
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "SpeechRecognitionParent::StreamResult",
        [self, payload = nsCString(aText), aFinal, aConfidence]() {
          LOGV("Sending streaming result: '{}' (final={}, conf={})",
               payload.get(), aFinal, aConfidence);
          if (self->CanSend()) {
            (void)self->SendOnRecognitionResult(payload, aFinal, aConfidence);
          }
        }));
  };

  // Drain the words the model finalized this step. They are already grouped at
  // word boundaries and carry per-word timing + confidence. Emit them as a
  // single final result with the mean confidence; the per-word timestamps are
  // logged (kept engine-internal — the Web Speech result has no per-word timing
  // field).
  auto emitFinalizedWords = [&]() {
    parakeet_stream_word* words = nullptr;
    int n = lib->parakeet_capi_stream_drain_words(mCapiStream, &words);
    if (n > 0) {
      nsCString text;
      float confSum = 0.0f;
      int counted = 0;
      for (int i = 0; i < n; ++i) {
        nsCString w(words[i].text ? words[i].text : "");
        stripTags(w);  // drop any inline <lang> markers
        w.Trim(" \t\n\r");
        if (w.IsEmpty()) {
          continue;
        }
        if (!text.IsEmpty()) {
          text.Append(' ');
        }
        text.Append(w);
        confSum += words[i].conf;
        ++counted;
        LOGV("  word '{}' [{:.2f}-{:.2f}] conf={:.2f}", w.get(), words[i].start,
             words[i].end, words[i].conf);
      }
      emit(text, /* isFinal */ true, counted ? confSum / counted : 1.0f);
    }
    lib->parakeet_capi_free_words(words, n > 0 ? n : 0);
  };

  nsTArray<float> chunk;

  while (mShouldContinueProcessing.load()) {
    size_t available = mAudioQueue.AvailableRead();
    if (available < minFeed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    size_t take = std::min(available, maxFeed);
    chunk.SetLength(take);
    size_t got = mAudioQueue.Dequeue(chunk.Elements(), AssertedCast<int>(take));
    chunk.SetLength(got);
    mProcessedAudioPos += got;

    int eou = 0;
    // The marker interval is the inference compute time; the text records the
    // audio fed and how much was queued (the buffering-latency component), so a
    // profile shows the real-time factor and end-to-end latency directly.
    TimeStamp feedStart = TimeStamp::Now();
    char* fed = lib->parakeet_capi_stream_feed(mCapiStream, chunk.Elements(),
                                               AssertedCast<int>(got), &eou);
    if (fed) {
      lib->parakeet_capi_free_string(fed);  // text comes from drain_words
    }
    PROFILER_MARKER_TEXT(
        "Parakeet stream_feed", MEDIA_PLAYBACK,
        MarkerOptions(MarkerTiming::IntervalUntilNowFrom(feedStart)),
        nsFmtCString("fed={:.0f}ms queued={:.0f}ms",
                     1000.0 * got / PARAKEET_SAMPLE_RATE,
                     1000.0 * available / PARAKEET_SAMPLE_RATE));
    emitFinalizedWords();
    (void)eou;
  }

  // Flush the end-of-stream tail, then emit its finalized words.
  char* tail = lib->parakeet_capi_stream_finalize(mCapiStream);
  if (tail) {
    lib->parakeet_capi_free_string(tail);
  }
  emitFinalizedWords();
  LOGD("Streaming loop exiting");
}

}  // namespace mozilla

#undef LOGV
#undef LOGD
#undef LOGE
