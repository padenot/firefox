/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8  et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionParent.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "SpeechRecognitionModels.h"
#include "mozIRemoteLazyInputStream.h"
#include "mozilla/Logging.h"
#include "mozilla/Mutex.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/hwinference/HWInferenceChild.h"
#include "mozilla/ipc/FileDescriptorUtils.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/llama/LlamaRuntimeLinker.h"
#include "mozilla/media/MediaUtils.h"
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
// Sample rate the Parakeet models operate at.
static constexpr int32_t PARAKEET_SAMPLE_RATE = 16000;
static constexpr int32_t DEFAULT_NUM_THREADS = 4;

SpeechRecognitionParent::ModelIdentifier
SpeechRecognitionParent::LanguagesToModelIdentifier(
    const nsTArray<nsCString>& aLanguages) {
  // Determine the primary-subtag locale prefix (e.g. "en" from "en-US").
  nsCString prefix;
  if (!aLanguages.IsEmpty()) {
    prefix = aLanguages[0];
    int32_t dash = prefix.FindChar('-');
    if (dash != kNotFound) {
      prefix.Truncate(dash);
    }
  }

  // A pref may override the default model for a locale prefix:
  // media.webspeech.recognition.model.<prefix> (or .multilingual for the
  // fallback). Empty prefix uses the multilingual fallback.
  nsAutoCString prefKey("media.webspeech.recognition.model.");
  prefKey.Append(prefix.IsEmpty() ? "multilingual"_ns : prefix);
  nsAutoCString prefModelId;
  Preferences::GetCString(prefKey.get(), prefModelId);

  auto toIdentifier = [](const dom::SpeechRecognitionModelInfo& m) {
    return ModelIdentifier{nsCString(m.repo), nsCString(m.filename),
                           nsCString(m.revision), m.size_mb};
  };

  if (!prefModelId.IsEmpty()) {
    for (const auto& m : dom::kSpeechRecognitionModels) {
      if (m.id && prefModelId.Equals(m.id)) {
        return toIdentifier(m);
      }
    }
    LOGD(
        "LanguagesToModelIdentifier: pref '{}' names unknown model '{}', "
        "ignoring",
        prefKey.get(), prefModelId.get());
  }

  // No usable pref: pick the default model whose locale list matches the
  // prefix, falling back to the default fallback model (empty locale list).
  const dom::SpeechRecognitionModelInfo* fallback = nullptr;
  for (const auto& m : dom::kSpeechRecognitionModels) {
    if (!m.id) {
      break;
    }
    if (!m.locales[0]) {
      if (m.is_default && !fallback) {
        fallback = &m;
      }
      continue;
    }
    for (const char* const* l = m.locales; *l; ++l) {
      if (prefix.IsEmpty() ||
          StringBeginsWith(prefix, nsDependentCString(*l))) {
        if (m.is_default) {
          return toIdentifier(m);
        }
      }
    }
  }

  if (fallback) {
    return toIdentifier(*fallback);
  }

  MOZ_ASSERT_UNREACHABLE("No default model found in kSpeechRecognitionModels");
  return {};
}

nsCString SpeechRecognitionParent::ModelIdentifier::ToString() const {
  return nsFmtCString("{}/{}/{}", mModelName.get(), mFileName.get(),
                      mRevision.get());
}

void SpeechRecognitionParent::ResolveOrRejectInitOnIPCThread(
    InitResolver&& aResolver, bool aSuccess) {
  if (!aSuccess) {
    // Init failed after this session claimed the single-session slot in
    // RecvInit. Release it here so the next session is not falsely rejected as
    // concurrent. The concurrent-session rejection path in RecvInit resolves
    // the resolver directly and never reaches this helper, so it cannot clear
    // another session's slot.
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session after init failure");
      sActiveSession = nullptr;
    }
  }
  // An empty string means success; otherwise it carries the Web Speech error
  // token. Every failure reaching this helper is a model-retrieval or
  // engine-startup problem, surfaced as "network" so it is not conflated with
  // the genuine concurrent-session rejection handled directly in RecvInit.
  nsCString error = aSuccess ? nsCString() : nsCString("network");
  if (GetActorEventTarget()->IsOnCurrentThread()) {
    LOGV("Resolving init on same thread, error='{}'", error.get());
    aResolver(error);
  } else {
    LOGV("Resolving init accross thread, error='{}'", error.get());
    GetActorEventTarget()->Dispatch(NS_NewRunnableFunction(
        "Speech recognition init runnable",
        [resolver = std::move(aResolver), error = std::move(error)]() {
          LOGV("Resolving init accross thread, error='{}'", error.get());
          resolver(error);
        }));
  }
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RunHWInferenceBoolQuery(
    const char* aFuncName,
    std::function<RefPtr<BoolPromise>(hwinference::HWInferenceChild*)>
        aSendFunc,
    std::function<void(const bool&)> aResolver,
    MozPromiseRequestHolder<BoolPromise>& aRequestHolder) {
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} No UtilityProcessChild available", aFuncName);
    aResolver(false);
    return IPC_OK();
  }

  mozilla::hwinference::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available", aFuncName);
    aResolver(false);
    return IPC_OK();
  }

  // Shared by both continuations below so the resolver is moved-from exactly
  // once, whichever one actually runs (MozPromise::Then() invokes only one of
  // the two, but both closures are constructed eagerly).
  auto resolver =
      MakeRefPtr<media::Refcountable<std::function<void(const bool&)>>>();
  *resolver = std::move(aResolver);

  aSendFunc(hwInferenceChild)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}, resolver, aFuncName,
           &aRequestHolder](bool aResult) mutable {
            aRequestHolder.Complete();
            LOGD("{} Sending response back to content process: {}", aFuncName,
                 aResult ? "true" : "false");
            (*resolver)(aResult);
          },
          [self = RefPtr{this}, resolver, aFuncName,
           &aRequestHolder](ResponseRejectReason aReason) mutable {
            aRequestHolder.Complete();
            LOGE("{} IPC call to main process failed: {}", aFuncName,
                 static_cast<int>(aReason));
            (*resolver)(false);
          })
      ->Track(aRequestHolder);

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvIsModelAvailable(
    const nsTArray<nsCString>& aLanguages,
    IsModelAvailableResolver&& aResolver) {
  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);
  LOGD("{} languages: {} mapped to model={}", __func__,
       fmt::join(aLanguages, ", "), modelIdentifier.ToString().get());

  return RunHWInferenceBoolQuery(
      __func__,
      [modelIdentifier](hwinference::HWInferenceChild* aChild) {
        return aChild->SendIsModelAvailable(
            "parakeet-gguf"_ns, modelIdentifier.mModelName,
            modelIdentifier.mRevision, modelIdentifier.mFileName);
      },
      std::move(aResolver), mIsModelAvailableRequest);
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvIsModelInstalled(
    const nsTArray<nsCString>& aLanguages,
    IsModelInstalledResolver&& aResolver) {
  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);
  LOGD("{} languages: {} mapped to model={}", __func__,
       fmt::join(aLanguages, ", "), modelIdentifier.ToString().get());

  return RunHWInferenceBoolQuery(
      __func__,
      [modelIdentifier](hwinference::HWInferenceChild* aChild) {
        return aChild->SendIsModelInstalled(
            "parakeet-gguf"_ns, modelIdentifier.mModelName,
            modelIdentifier.mRevision, modelIdentifier.mFileName);
      },
      std::move(aResolver), mIsModelInstalledRequest);
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInstallModels(
    const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver) {
  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);
  LOGD("{} languages: {} mapped to model={}", __func__,
       fmt::join(aLanguages, ", "), modelIdentifier.ToString().get());

  return RunHWInferenceBoolQuery(
      __func__,
      [modelIdentifier](hwinference::HWInferenceChild* aChild) {
        return aChild->SendInstallModel(
            "speech-recognition"_ns, modelIdentifier.mModelName,
            modelIdentifier.mRevision, modelIdentifier.mFileName);
      },
      std::move(aResolver), mInstallModelRequest);
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvGetModelDownloadSize(
    const nsTArray<nsCString>& aLanguages,
    GetModelDownloadSizeResolver&& aResolver) {
  aResolver(LanguagesToModelIdentifier(aLanguages).mSizeMB);
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
      mProcessedAudioPos(0),
      mTimingLock("SpeechRecognitionParent::mTimingLock") {
  // MOZ_DUMP_AUDIO=1 MOZ_DISABLE_UTILITY_SANDBOX=1 to activate this
  // It will contain the (repeating segments of audio), precisely that has been
  // sent to the recognizer.
  const int MONO = 1;
  mRecognitionAudioDumper.Open("SpeechRecognition-Audio-Input", MONO,
                               PARAKEET_SAMPLE_RATE);

  // Load tunable parameters from preferences (can be overridden via
  // about:config)
  LoadPreferences();
}

void SpeechRecognitionParent::LoadPreferences() {
  // Timing parameters
  mParams.mRecognitionIntervalMs =
      Preferences::GetInt("media.webspeech.recognition.interval_ms", 500);
  mParams.mKeepAudioMs =
      Preferences::GetInt("media.webspeech.recognition.keep_audio_ms", 200);

  // VAD parameters (not wired up yet)
  mParams.mUseVAD =
      Preferences::GetBool("media.webspeech.recognition.use_vad", false);
  mParams.mVADThreshold =
      Preferences::GetFloat("media.webspeech.recognition.vad_threshold", 0.6f);
  mParams.mVADMinSpeechMs =
      Preferences::GetInt("media.webspeech.recognition.vad_min_speech_ms", 250);
  mParams.mVADMinSilenceMs = Preferences::GetInt(
      "media.webspeech.recognition.vad_min_silence_ms", 2000);
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

  // Shared by both continuations below so the resolver is moved-from exactly
  // once, whichever one actually runs (MozPromise::Then() invokes only one of
  // the two, but both closures are constructed eagerly).
  auto resolver = MakeRefPtr<media::Refcountable<InitResolver>>();
  *resolver = std::move(aResolver);

  hwInferenceChild
      ->SendGetModelFile("parakeet-gguf"_ns, "speech-recognition"_ns,
                         modelIdentifier.mModelName, modelIdentifier.mRevision,
                         modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}, resolver](
              const mozilla::hwinference::GetModelFileResult& aResult) mutable {
            self->mGetModelFileRequest.Complete();
            if (aResult.type() ==
                mozilla::hwinference::GetModelFileResult::TGetModelError) {
              LOGE("{} GetModelError with nsresult={:x}", __func__,
                   static_cast<uint32_t>(
                       aResult.get_GetModelError().errorCode()));
              self->ResolveOrRejectInitOnIPCThread(std::move(*resolver), false);
              return;
            }

            // Convert FileDescriptor to FILE* using the helper function
            mozilla::ipc::FileDescriptor fd =
                aResult.get_GetModelFileSuccess().fd();

            FILE* file = FileDescriptorToFILE(fd, "rb");
            if (!file) {
              LOGE("{} Failed to convert FileDescriptor to FILE*", __func__);
              self->ResolveOrRejectInitOnIPCThread(std::move(*resolver), false);
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
                    "Initialize parakeet context", [self, resolver]() mutable {
                      self->InitializeParakeetContext(std::move(*resolver));
                    }));
            if (NS_FAILED(rv)) {
              LOGE("Failed to create recognition thread: {:x}",
                   static_cast<uint32_t>(rv));
              self->ResolveOrRejectInitOnIPCThread(std::move(*resolver), false);
            }
          },
          [self = RefPtr{this},
           resolver](mozilla::ipc::ResponseRejectReason aReason) mutable {
            self->mGetModelFileRequest.Complete();
            LOGE("{} Promise rejected with reason {}", __func__,
                 static_cast<int>(aReason));
            self->ResolveOrRejectInitOnIPCThread(std::move(*resolver), false);
          })
      ->Track(mGetModelFileRequest);
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

  // Route ggml logs through gSpeechRecognitionParentLog instead of its
  // default unconditional stderr logging.
  lib->llama_log_set(
      [](ggml_log_level level, const char* text, void* /* user_data */) {
        switch (level) {
          case GGML_LOG_LEVEL_NONE:
            MOZ_LOG(gSpeechRecognitionParentLog, LogLevel::Disabled,
                    ("%s", text));
            break;
          case GGML_LOG_LEVEL_DEBUG:
            MOZ_LOG(gSpeechRecognitionParentLog, LogLevel::Debug, ("%s", text));
            break;
          case GGML_LOG_LEVEL_INFO:
            MOZ_LOG(gSpeechRecognitionParentLog, LogLevel::Info, ("%s", text));
            break;
          case GGML_LOG_LEVEL_WARN:
            MOZ_LOG(gSpeechRecognitionParentLog, LogLevel::Warning,
                    ("%s", text));
            break;
          case GGML_LOG_LEVEL_ERROR:
            MOZ_LOG(gSpeechRecognitionParentLog, LogLevel::Error, ("%s", text));
            break;
          default:
            MOZ_LOG(gSpeechRecognitionParentLog, LogLevel::Verbose,
                    ("%s", text));
            break;
        }
      },
      nullptr);

  // Test-only: widen the window before mLock is acquired below, so a test
  // can deterministically land ActorDestroy() (running on another thread)
  // in that window instead of relying on scheduling luck.
  int32_t testDelayMs =
      StaticPrefs::media_webspeech_recognition_testing_parakeet_init_delay_ms();
  if (testDelayMs > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(testDelayMs));
  }

  // ActorDestroy() can run concurrently on the main thread while this is
  // delayed above (or otherwise still in flight). Bail out instead of
  // resurrecting mShouldContinueProcessing and starting a streaming loop
  // nobody will ever stop, or touching mModelFile after ActorDestroy has
  // cleared it.
  if (mActorDestroyed.load()) {
    LOGD("{} Actor already destroyed, abandoning init", __func__);
    return;
  }

  FILE* modelFile = nullptr;
  nsCString language;
  {
    MutexAutoLock lock(mLock);
    modelFile = mModelFile.get();
    language = mLanguage;
  }

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

  // Clear active session if this was it. The actor can be torn down without
  // RecvStop() ever running (e.g. a detached frame), which would otherwise
  // leave sActiveSession dangling and reject every subsequent session as
  // concurrent.
  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session in ActorDestroy");
      sActiveSession = nullptr;
    }
  }

  mShouldContinueProcessing.store(false);
  mActorDestroyed.store(true);

  // Disconnect outstanding requests to the utility process so their
  // resolve/reject callbacks never run and try to resolve a dead IPDL
  // resolver after this actor is torn down.
  mIsModelAvailableRequest.DisconnectIfExists();
  mIsModelInstalledRequest.DisconnectIfExists();
  mInstallModelRequest.DisconnectIfExists();
  mGetModelFileRequest.DisconnectIfExists();

  // Shutdown() would join the recognition thread by spinning a nested native
  // event loop on the calling thread; called from here (already inside an
  // IPC message dispatch on the main thread), that reentrant pump can crash.
  // AsyncShutdown() only requests shutdown, so the thread's own queue drains
  // (ProcessAudioStreaming() exits once it observes mShouldContinueProcessing
  // above, or InitializeParakeetContext() bails out on mActorDestroyed) and
  // it winds itself down without blocking here. mCapiCtx/mCapiStream are
  // freed by the recognition thread itself as the last thing it does, since
  // freeing them from here could race with that thread still using them.
  if (mRecognitionThread) {
    mRecognitionThread->AsyncShutdown();
    mRecognitionThread = nullptr;
  }

  {
    MutexAutoLock lock(mLock);
    if (mModelFile) {
      mModelFile = nullptr;
    }
  }
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
      aResolver("concurrent-session"_ns);
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
    nsTArray<float>&& aAudioData, const TimeStamp& aCaptureEndTime) {
  LOGV("{} {} samples", __func__, aAudioData.Length());

  size_t length = aAudioData.Length();
  if (!mAudioQueue.Enqueue(aAudioData.Elements(), static_cast<int>(length))) {
    LOGD("Audio queue full, dropping sample");
  }

  {
    MutexAutoLock lock(mTimingLock);
    mEnqueuedAudioPos += length;
    mCaptureTimeSamples.push_back({mEnqueuedAudioPos, aCaptureEndTime});
  }

  return IPC_OK();
}

TimeStamp SpeechRecognitionParent::CaptureTimeForPosition(size_t aPosition) {
  MutexAutoLock lock(mTimingLock);
  // Drop samples that are behind aPosition, but always keep at least one to
  // extrapolate from.
  while (mCaptureTimeSamples.size() > 1 &&
         mCaptureTimeSamples.front().mPosition < aPosition) {
    mCaptureTimeSamples.pop_front();
  }
  if (mCaptureTimeSamples.empty()) {
    return TimeStamp::Now();
  }
  const CaptureTimeSample& sample = mCaptureTimeSamples.front();
  return EstimateSampleTimeStamp(int64_t(sample.mPosition), sample.mTimeStamp,
                                 int64_t(aPosition), PARAKEET_SAMPLE_RATE);
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

void SpeechRecognitionParent::ProcessAudioStreaming() {
  LOGD("{} Starting cache-aware streaming loop", __func__);

  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();

  // The model keeps its own encoder/decoder caches across feeds, so we just
  // hand it new audio as it arrives. parakeet_capi_stream_feed returns the
  // text newly committed by this feed (a cache-aware transducer never revises
  // past output). Each committed delta is emitted as a final result at
  // streaming latency.
  // Feed promptly: the content process already streams audio in small blocks,
  // and the model buffers internally until its chunk fills, so we just forward
  // whatever has arrived. A small floor avoids spinning on sub-block wakeups.
  const size_t minFeed = size_t(0.01 * PARAKEET_SAMPLE_RATE);  // 10 ms
  const size_t maxFeed = size_t(PARAKEET_SAMPLE_RATE);         // 1 s

  // Strip inline <...> markers (e.g. nemotron <en-US> language tags).
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
                                    float aConfidence, TimeStamp aEventTime) {
    if (aText.IsEmpty()) {
      return;
    }
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "SpeechRecognitionParent::StreamResult",
        [self, payload = nsCString(aText), aFinal, aConfidence, aEventTime]() {
          LOGV("Sending streaming result: '{}' (final={}, conf={})",
               payload.get(), aFinal, aConfidence);
          if (self->CanSend()) {
            (void)self->SendOnRecognitionResult(payload, aFinal, aConfidence,
                                                aEventTime);
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
      emit(text, /* isFinal */ true, counted ? confSum / counted : 1.0f,
           CaptureTimeForPosition(mProcessedAudioPos));
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

    // Dump audio for debugging
    mRecognitionAudioDumper.Write(chunk.Elements(), chunk.Length());

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

  // Freed here, on the thread that alone uses them, rather than from
  // ActorDestroy() on the main thread: ActorDestroy() only requests this
  // thread's shutdown (see AsyncShutdown() there) instead of blocking on it,
  // so it can't assume the loop above has already exited.
  if (mCapiStream) {
    lib->parakeet_capi_stream_free(mCapiStream);
    mCapiStream = nullptr;
  }
  if (mCapiCtx) {
    lib->parakeet_capi_free(mCapiCtx);
    mCapiCtx = nullptr;
  }
}

}  // namespace mozilla

#undef LOGV
#undef LOGD
#undef LOGE
