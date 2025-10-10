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
#include "mozilla/StaticPtr.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/FileDescriptorUtils.h"
#include "mozilla/hwinference/HWInferenceChild.h"
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
    mozilla::llama::LlamaLibWrapper* lib = mozilla::llama::LlamaRuntimeLinker::Get();
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

SpeechRecognitionParent::ModelIdentifier
SpeechRecognitionParent::LanguagesToModelIdentifier(
    const nsTArray<nsCString>&) {
  return {"cstr/parakeet-tdt-0.6b-v3-GGUF"_ns,
          "parakeet-tdt-0.6b-v3-q4_k.gguf"_ns, "main"_ns};
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
      ->SendIsModelAvailable("parakeet-gguf"_ns,
                             modelIdentifier.mModelName,
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
    : mLock("SpeechRecognitionLock") {
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
    modelIdentifier =
        LanguagesToModelIdentifier(nsTArray{mLanguage});
  }

  LOGD("{} Requesting model: model={}", __func__,
       modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendGetModelFile(
        "parakeet-gguf"_ns,
        "speech-recognition"_ns,
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

  mozilla::llama::LlamaLibWrapper* lib = mozilla::llama::LlamaRuntimeLinker::Get();
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
  {
    MutexAutoLock lock(mLock);
    modelFile = mModelFile.get();
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

  MutexAutoLock lock(mLock);
  if (mModelFile) {
    mModelFile = nullptr;
  }

  if (mRecognitionThread) {
    mRecognitionThread->Shutdown();
    mRecognitionThread = nullptr;
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

void SpeechRecognitionParent::ProcessAudioOnBackgroundThread() {
}

}  // namespace mozilla

#undef LOGV
#undef LOGD
#undef LOGE
