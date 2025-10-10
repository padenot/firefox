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
      mThreadRunning(false),
      mParams() {  // Initialize with defaults


  // Load tunable parameters from preferences (can be overridden via
  // about:config)
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
}

}  // namespace mozilla::ipc

#undef LOGV
#undef LOGD
#undef LOGE
