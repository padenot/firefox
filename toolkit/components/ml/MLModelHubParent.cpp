/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MLModelHubParent.h"
#include "mozilla/Logging.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/FileBlobImpl.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/ErrorResult.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsLocalFile.h"
#include <functional>

namespace mozilla::ml {

static LazyLogModule sMLModelHubParentLog("MLModelHubParent");
#define PARENT_LOG(level, msg, ...) \
  MOZ_LOG_FMT(sMLModelHubParentLog, level, msg, ##__VA_ARGS__)

// Promise handler for async model availability checking
class PromiseHandler final : public dom::PromiseNativeHandler {
 public:
  NS_DECL_ISUPPORTS

  explicit PromiseHandler(
      std::function<void(JSContext*, JS::Handle<JS::Value>)> aResolvedCallback,
      std::function<void(JSContext*, JS::Handle<JS::Value>)> aRejectedCallback)
      : mResolvedCallback(std::move(aResolvedCallback)),
        mRejectedCallback(std::move(aRejectedCallback)) {}

  MOZ_CAN_RUN_SCRIPT
  void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    if (mResolvedCallback) {
      mResolvedCallback(aCx, aValue);
    }
  }

  MOZ_CAN_RUN_SCRIPT
  void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    if (mRejectedCallback) {
      mRejectedCallback(aCx, aValue);
    }
  }

 private:
  ~PromiseHandler() = default;

  std::function<void(JSContext*, JS::Handle<JS::Value>)> mResolvedCallback;
  std::function<void(JSContext*, JS::Handle<JS::Value>)> mRejectedCallback;
};

NS_IMPL_ISUPPORTS(PromiseHandler, dom::PromiseNativeHandler)

// ModelHubProgressCallback implementation
NS_IMPL_ISUPPORTS(ModelHubProgressCallback, nsIMLModelDownloadProgressCallback)

ModelHubProgressCallback::ModelHubProgressCallback(PMLModelHubParent* aParent,
                                                   const nsCString& aSessionId)
    : mParent(aParent), mSessionId(aSessionId) {}

NS_IMETHODIMP
ModelHubProgressCallback::OnProgress(int32_t aProgress, int64_t aCurrentLoaded,
                                     int64_t aTotalLoaded, int64_t aTotal) {
  PARENT_LOG(LogLevel::Debug, "Progress callback: session={} progress={}",
             mSessionId.get(), aProgress);

  if (mParent) {
    static_cast<MLModelHubParent*>(mParent.get())
        ->NotifyProgress(mSessionId, aProgress, aCurrentLoaded, aTotalLoaded,
                         aTotal);
  }
  return NS_OK;
}

// ModelHubCompletionCallback implementation
NS_IMPL_ISUPPORTS(ModelHubCompletionCallback,
                  nsIMLModelDownloadCompletionCallback)

ModelHubCompletionCallback::ModelHubCompletionCallback(
    PMLModelHubParent* aParent, const nsCString& aSessionId)
    : mParent(aParent), mSessionId(aSessionId) {}

NS_IMETHODIMP
ModelHubCompletionCallback::OnSuccess(const nsAString& aModel,
                                      const nsAString& aRevision) {
  PARENT_LOG(LogLevel::Debug, "Completion success callback: session={}",
             mSessionId.get());

  if (mParent) {
    static_cast<MLModelHubParent*>(mParent.get())
        ->NotifyComplete(mSessionId, NS_ConvertUTF16toUTF8(aModel),
                         NS_ConvertUTF16toUTF8(aRevision));
  }
  return NS_OK;
}

NS_IMETHODIMP
ModelHubCompletionCallback::OnError(const nsAString& aError) {
  PARENT_LOG(LogLevel::Debug, "Completion error callback: session={} error={}",
             mSessionId.get(), NS_ConvertUTF16toUTF8(aError).get());

  if (mParent) {
    static_cast<MLModelHubParent*>(mParent.get())
        ->NotifyError(mSessionId, NS_ConvertUTF16toUTF8(aError));
  }
  return NS_OK;
}

/* static */
already_AddRefed<MLModelHubParent> MLModelHubParent::Create() {
  PARENT_LOG(LogLevel::Debug, "MLModelHubParent::Create() called");
  RefPtr<MLModelHubParent> parent = new MLModelHubParent();
  PARENT_LOG(LogLevel::Debug,
             "MLModelHubParent::Create() returning new instance");
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "MLModelHubParent::InitializeService", [self = RefPtr{parent}]() {
        PARENT_LOG(
            LogLevel::Debug,
            "MLModelHubParent: Getting XPCOM ModelHub service on main thread");
        self->mModelHubService = do_GetService("@mozilla.org/ml-modelhub;1");
        if (!self->mModelHubService) {
          PARENT_LOG(
              LogLevel::Error,
              "MLModelHubParent: Failed to get MLModelHub XPCOM service!");
        } else {
          PARENT_LOG(
              LogLevel::Debug,
              "MLModelHubParent: Successfully got MLModelHub XPCOM service");
        }
      }));
  return parent.forget();
}

mozilla::ipc::IPCResult MLModelHubParent::RecvIsModelAvailable(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    IsModelAvailableResolver&& aResolver) {
  PARENT_LOG(LogLevel::Debug, "RecvIsModelAvailable: model={} revision={} filename={}",
             aModel.get(), aRevision.get(), aFilename.get());

  // Dispatch to main thread to handle XPCOM service call
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "MLModelHubParent::RecvIsModelAvailable",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), filename = std::move(aFilename),
       resolver = std::move(aResolver)]() mutable {
        ModelAvailabilityResult result;
        result.available() = false;

        if (!self->mModelHubService) {
          PARENT_LOG(LogLevel::Error,
                     "RecvIsModelAvailable: ModelHub service not available!");
          result.error() = "ModelHub service not available";
          resolver(result);
          return;
        }

        PARENT_LOG(
            LogLevel::Debug,
            "RecvIsModelAvailable: On main thread, calling ModelHub service");

        // Get the promise from the async isModelAvailable call
        RefPtr<dom::Promise> promise;
        nsresult rv = self->mModelHubService->IsModelAvailable(
            NS_ConvertUTF8toUTF16(model), NS_ConvertUTF8toUTF16(revision),
            NS_ConvertUTF8toUTF16(filename), getter_AddRefs(promise));

        if (NS_FAILED(rv) || !promise) {
          PARENT_LOG(LogLevel::Error,
                     "RecvIsModelAvailable: Failed to get Promise, nsresult={}",
                     static_cast<uint32_t>(rv));
          result.available() = false;
          result.error() = "Failed to create availability check";
          resolver(result);
          return;
        }

        PARENT_LOG(
            LogLevel::Debug,
            "RecvIsModelAvailable: Got Promise, setting up resolution handler");

        // Set up Promise resolution handlers
        promise->AppendNativeHandler(new PromiseHandler(
            [resolver = std::move(resolver)](
                JSContext* aCx, JS::Handle<JS::Value> aValue) mutable {
              // Success handler
              PARENT_LOG(LogLevel::Debug,
                         "RecvIsModelAvailable: Promise resolved successfully");

              ModelAvailabilityResult result;
              bool available = false;

              if (aValue.isBoolean()) {
                available = aValue.toBoolean();
                PARENT_LOG(LogLevel::Debug,
                           "RecvIsModelAvailable: Model availability = {}",
                           available);
              } else {
                PARENT_LOG(LogLevel::Warning,
                           "RecvIsModelAvailable: Promise resolved with "
                           "non-boolean value");
              }

              result.available() = available;
              result.error() = "";
              resolver(result);
            },
            [resolver = std::move(resolver)](
                JSContext* aCx, JS::Handle<JS::Value> aValue) mutable {
              // Error handler
              PARENT_LOG(LogLevel::Error,
                         "RecvIsModelAvailable: Promise rejected");

              ModelAvailabilityResult result;
              result.available() = false;
              result.error() = "Model availability check failed";
              resolver(result);
            }));
      }));

  return IPC_OK();
}

mozilla::ipc::IPCResult MLModelHubParent::RecvStartModelDownload(
    nsCString&& aTaskName, nsCString&& aModel, nsCString&& aRevision,
    nsTArray<nsCString>&& aFiles, StartModelDownloadResolver&& aResolver) {
  PARENT_LOG(LogLevel::Debug,
             "RecvStartModelDownload: model={} revision={} files={}",
             aModel.get(), aRevision.get(), aFiles.Length());

  // Dispatch to main thread to handle XPCOM service call
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "MLModelHubParent::RecvStartModelDownload",
      [self = RefPtr(this), taskName = std::move(aTaskName),
       model = std::move(aModel), revision = std::move(aRevision),
       files = std::move(aFiles), resolver = std::move(aResolver)]() mutable {
        if (!self->mModelHubService) {
          PARENT_LOG(LogLevel::Error,
                     "RecvStartModelDownload: ModelHub service not available!");
          resolver(""_ns);
          return;
        }

        PARENT_LOG(
            LogLevel::Debug,
            "RecvStartModelDownload: On main thread, calling ModelHub service");

        // Create callbacks
        RefPtr<ModelHubProgressCallback> progressCallback =
            new ModelHubProgressCallback(self.get(),
                                         ""_ns);  // sessionId will be updated
        RefPtr<ModelHubCompletionCallback> completionCallback =
            new ModelHubCompletionCallback(self.get(),
                                           ""_ns);  // sessionId will be updated

        // Convert files array
        nsTArray<nsString> convertedFiles;
        for (const auto& file : files) {
          convertedFiles.AppendElement(NS_ConvertUTF8toUTF16(file));
        }

        // Start the download
        nsString sessionId;
        nsresult rv = self->mModelHubService->DownloadModel(
            NS_ConvertUTF8toUTF16(taskName), NS_ConvertUTF8toUTF16(model),
            NS_ConvertUTF8toUTF16(revision), convertedFiles, progressCallback,
            completionCallback, sessionId);

        if (NS_FAILED(rv)) {
          PARENT_LOG(LogLevel::Error, "Failed to start model download: {}",
                     static_cast<uint32_t>(rv));
          resolver(""_ns);
          return;
        }

        nsCString sessionIdUTF8 = NS_ConvertUTF16toUTF8(sessionId);

        // Update callback session IDs
        progressCallback->mSessionId = sessionIdUTF8;
        completionCallback->mSessionId = sessionIdUTF8;

        // Store callbacks for this session
        self->mProgressCallbacks.InsertOrUpdate(sessionIdUTF8,
                                                progressCallback);
        self->mCompletionCallbacks.InsertOrUpdate(sessionIdUTF8,
                                                  completionCallback);

        PARENT_LOG(LogLevel::Debug, "Started download with session ID: {}",
                   sessionIdUTF8.get());
        resolver(sessionIdUTF8);
      }));

  return IPC_OK();
}

mozilla::ipc::IPCResult MLModelHubParent::RecvGetModelBlob(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFile,
    GetModelBlobResolver&& aResolver) {
  PARENT_LOG(LogLevel::Debug,
             "RecvGetModelBlob: model={} revision={} file={}", aModel.get(),
             aRevision.get(), aFile.get());

  // Dispatch to main thread to handle XPCOM service call
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "MLModelHubParent::RecvGetModelBlob",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), file = std::move(aFile),
       resolver = std::move(aResolver)]() mutable {
        if (!self->mModelHubService) {
          PARENT_LOG(LogLevel::Error,
                     "RecvGetModelBlob: ModelHub service not available!");
          ModelBlobResult result;
          result.success() = false;
          result.error() = "ModelHub service not available"_ns;
          result.blob() = Nothing();
          resolver(result);
          return;
        }

        PARENT_LOG(
            LogLevel::Debug,
            "RecvGetModelBlob: On main thread, calling ModelHub service");

        // First, check if we have a getModelBlob method, otherwise fall back to getModelFilePath
        // For now, assume we need to use the old interface and create a blob from file path
        nsString filePath;
        nsresult rv = self->mModelHubService->GetModelFilePath(
            NS_ConvertUTF8toUTF16(model), NS_ConvertUTF8toUTF16(revision),
            NS_ConvertUTF8toUTF16(file), filePath);

        ModelBlobResult result;
        if (NS_FAILED(rv) || filePath.IsEmpty()) {
          PARENT_LOG(LogLevel::Error, "RecvGetModelBlob: Failed to get model file path");
          result.success() = false;
          result.error() = "Failed to get model file path"_ns;
          result.blob() = Nothing();
          resolver(result);
          return;
        }

        // Convert file path to blob
        PARENT_LOG(LogLevel::Debug, "RecvGetModelBlob: Got model file path: {}, creating blob",
                   NS_ConvertUTF16toUTF8(filePath).get());

        nsCOMPtr<nsIFile> modelFile;
        rv = NS_NewLocalFile(filePath, getter_AddRefs(modelFile));
        if (NS_FAILED(rv) || !modelFile) {
          PARENT_LOG(LogLevel::Error, "RecvGetModelBlob: Failed to create nsIFile from path");
          result.success() = false;
          result.error() = "Failed to create file object"_ns;
          result.blob() = Nothing();
          resolver(result);
          return;
        }

        RefPtr<mozilla::dom::BlobImpl> blobImpl = new mozilla::dom::FileBlobImpl(modelFile);
        if (!blobImpl) {
          PARENT_LOG(LogLevel::Error, "RecvGetModelBlob: Failed to create BlobImpl");
          result.success() = false;
          result.error() = "Failed to create blob implementation"_ns;
          result.blob() = Nothing();
          resolver(result);
          return;
        }

        mozilla::dom::IPCBlob ipcBlob;
        rv = mozilla::dom::IPCBlobUtils::Serialize(blobImpl, ipcBlob);
        if (NS_FAILED(rv)) {
          PARENT_LOG(LogLevel::Error, "RecvGetModelBlob: Failed to serialize blob to IPCBlob");
          result.success() = false;
          result.error() = "Failed to serialize blob"_ns;
          result.blob() = Nothing();
          resolver(result);
          return;
        }

        PARENT_LOG(LogLevel::Debug, "RecvGetModelBlob: Successfully created and serialized blob");
        result.success() = true;
        result.error() = ""_ns;
        result.blob() = Some(ipcBlob);
        resolver(result);
      }));

  return IPC_OK();
}

void MLModelHubParent::ActorDestroy(ActorDestroyReason aReason) {
  PARENT_LOG(LogLevel::Debug, "MLModelHubParent::ActorDestroy reason={}",
             static_cast<int>(aReason));

  // Clear callback references
  mProgressCallbacks.Clear();
  mCompletionCallbacks.Clear();
}

void MLModelHubParent::NotifyProgress(const nsCString& aSessionId,
                                      int32_t aProgress, int64_t aCurrentLoaded,
                                      int64_t aTotalLoaded, int64_t aTotal) {
  PARENT_LOG(LogLevel::Debug, "NotifyProgress: session={} progress={}",
             aSessionId.get(), aProgress);

  if (CanSend()) {
    ModelDownloadProgress progress;
    progress.progress() = aProgress;
    progress.currentLoaded() = aCurrentLoaded;
    progress.totalLoaded() = aTotalLoaded;
    progress.total() = aTotal;

    Unused << SendOnModelDownloadProgress(aSessionId, progress);
  }
}

void MLModelHubParent::NotifyComplete(const nsCString& aSessionId,
                                      const nsCString& aModel,
                                      const nsCString& aRevision) {
  PARENT_LOG(LogLevel::Debug, "NotifyComplete: session={} model={}",
             aSessionId.get(), aModel.get());

  if (CanSend()) {
    ModelDownloadResult result;
    result.success() = true;
    result.sessionId() = aSessionId;
    result.error() = "";

    Unused << SendOnModelDownloadComplete(aSessionId, result);
  }

  // Clean up callbacks for this session
  mProgressCallbacks.Remove(aSessionId);
  mCompletionCallbacks.Remove(aSessionId);
}

void MLModelHubParent::NotifyError(const nsCString& aSessionId,
                                   const nsCString& aError) {
  PARENT_LOG(LogLevel::Error, "NotifyError: session={} error={}",
             aSessionId.get(), aError.get());

  if (CanSend()) {
    ModelDownloadResult result;
    result.success() = false;
    result.sessionId() = aSessionId;
    result.error() = aError;

    Unused << SendOnModelDownloadComplete(aSessionId, result);
  }

  // Clean up callbacks for this session
  mProgressCallbacks.Remove(aSessionId);
  mCompletionCallbacks.Remove(aSessionId);
}

// MLModelHubManager implementation
static StaticRefPtr<MLModelHubManager> sMLModelHubManagerInstance;
static StaticMutex sMLModelHubManagerMutex;

/* static */
MLModelHubManager& MLModelHubManager::GetSingleton() {
  StaticMutexAutoLock lock(sMLModelHubManagerMutex);
  if (!sMLModelHubManagerInstance) {
    sMLModelHubManagerInstance = new MLModelHubManager();
    ClearOnShutdown(&sMLModelHubManagerInstance);
  }
  return *sMLModelHubManagerInstance;
}

already_AddRefed<MLModelHubParent> MLModelHubManager::GetOrCreateParent() {
  PARENT_LOG(LogLevel::Debug, "MLModelHubManager::GetOrCreateParent called");
  MutexAutoLock lock(mMutex);

  if (!mParent) {
    PARENT_LOG(LogLevel::Debug,
               "MLModelHubManager: Creating new MLModelHubParent instance");
    mParent = MLModelHubParent::Create();
    PARENT_LOG(LogLevel::Debug,
               "MLModelHubManager: Created new MLModelHubParent instance");
  } else {
    PARENT_LOG(LogLevel::Debug,
               "MLModelHubManager: Reusing existing MLModelHubParent instance");
  }

  RefPtr<MLModelHubParent> parent = mParent;
  PARENT_LOG(LogLevel::Debug,
             "MLModelHubManager::GetOrCreateParent returning parent instance");
  return parent.forget();
}

void MLModelHubManager::Shutdown() {
  MutexAutoLock lock(mMutex);
  mParent = nullptr;
  PARENT_LOG(LogLevel::Debug, "MLModelHubManager shutdown complete");
}

}  // namespace mozilla::ml
