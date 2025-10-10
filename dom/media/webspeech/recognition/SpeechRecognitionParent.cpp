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
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/StaticPtr.h"
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

SpeechRecognitionParent::ModelIdentifier
SpeechRecognitionParent::LanguagesToModelIdentifier(
    const nsTArray<nsCString>&) {
  // mudler/parakeet.cpp cache-aware streaming GGUF, hosted on the Mozilla
  // model hub under asr-test/parakeet.
  return {"asr-test/parakeet"_ns, "realtime_eou_120m-v1-q5_k.gguf"_ns,
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

SpeechRecognitionParent::SpeechRecognitionParent()
    : mLock("SpeechRecognitionLock"), mShouldContinueProcessing(false) {}

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

  mShouldContinueProcessing.store(false);
  mActorDestroyed.store(true);

  // Disconnect outstanding requests to the utility process so their
  // resolve/reject callbacks never run and try to resolve a dead IPDL
  // resolver after this actor is torn down.
  mIsModelAvailableRequest.DisconnectIfExists();
  mInstallModelRequest.DisconnectIfExists();
  mGetModelFileRequest.DisconnectIfExists();

  // Shutdown() joins the recognition thread, which can be blocked trying to
  // acquire mLock in InitializeParakeetContext. Join before taking the lock,
  // or that thread can never make progress and Shutdown() never returns.
  if (mRecognitionThread) {
    mRecognitionThread->Shutdown();
    mRecognitionThread = nullptr;
  }

  {
    MutexAutoLock lock(mLock);
    if (mModelFile) {
      mModelFile = nullptr;
    }
  }

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

void SpeechRecognitionParent::ProcessAudioStreaming() {}

}  // namespace mozilla

#undef LOGV
#undef LOGD
#undef LOGE
