/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MLModelHubChild.h"
#include "mozilla/Logging.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/ClearOnShutdown.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace ml {

static LazyLogModule sMLModelHubChildLog("MLModelHubChild");
#define LOG(level, msg, ...) MOZ_LOG(sMLModelHubChildLog, level, (msg, ##__VA_ARGS__))

// MLModelHubChild implementation
MLModelHubChild::MLModelHubChild() {
  LOG(LogLevel::Debug, "MLModelHubChild::MLModelHubChild() constructor called");
}

/* static */
already_AddRefed<MLModelHubChild> MLModelHubChild::Create() {
  LOG(LogLevel::Debug, "MLModelHubChild::Create() called");
  RefPtr<MLModelHubChild> child = new MLModelHubChild();
  LOG(LogLevel::Debug, "MLModelHubChild::Create() returning child instance");
  return child.forget();
}

RefPtr<ModelAvailabilityPromise> MLModelHubChild::IsModelAvailable(
    const nsCString& aModel, const nsCString& aRevision, const nsCString& aFilename) {
  LOG(LogLevel::Debug, "IsModelAvailable: model=%s revision=%s",
      aModel.get(), aRevision.get());

  if (!CanSend()) {
    return ModelAvailabilityPromise::CreateAndReject(
        "MLModelHubChild not connected"_ns, __func__);
  }

  return SendIsModelAvailable(aModel, aRevision, aFilename)
      ->Then(GetCurrentSerialEventTarget(), __func__,
             [](const ModelAvailabilityResult& aResult) {
               if (aResult.available()) {
                 return ModelAvailabilityPromise::CreateAndResolve(true, __func__);
               }
               return ModelAvailabilityPromise::CreateAndReject(
                   aResult.error(), __func__);
             },
             [](ResponseRejectReason aReason) {
               return ModelAvailabilityPromise::CreateAndReject(
                   "IPC failed"_ns, __func__);
             });
}

RefPtr<ModelDownloadPromise> MLModelHubChild::DownloadModel(
    const nsCString& aTaskName, const nsCString& aModel, const nsCString& aRevision,
    const nsTArray<nsCString>& aFiles, ModelDownloadProgressCallback* aProgressCallback) {
  LOG(LogLevel::Debug, "DownloadModel: model=%s revision=%s files=%zu",
      aModel.get(), aRevision.get(), aFiles.Length());

  if (!CanSend()) {
    return ModelDownloadPromise::CreateAndReject(
        "MLModelHubChild not connected"_ns, __func__);
  }

  RefPtr<ModelDownloadPromise::Private> promise =
      new ModelDownloadPromise::Private(__func__);

  SendStartModelDownload(aTaskName, aModel, aRevision,
                        nsTArray<nsCString>(aFiles.Clone()))
      ->Then(GetCurrentSerialEventTarget(), __func__,
             [self = RefPtr{this}, promise, aProgressCallback](const nsCString& aSessionId) {
               if (aSessionId.IsEmpty()) {
                 promise->Reject("Failed to start download"_ns, __func__);
               } else {
                 // Store the pending download request
                 self->mPendingDownloads.InsertOrUpdate(
                     aSessionId, MakeUnique<PendingDownload>(promise, aProgressCallback));
                 LOG(LogLevel::Debug, "Started download with session ID: %s",
                     aSessionId.get());
               }
             },
             [promise](ResponseRejectReason aReason) {
               promise->Reject("IPC failed"_ns, __func__);
             });

  return promise;
}

RefPtr<ModelFilePathPromise> MLModelHubChild::GetModelFilePath(
    const nsCString& aModel, const nsCString& aRevision, const nsCString& aFile) {
  LOG(LogLevel::Debug, "GetModelFilePath: model=%s revision=%s file=%s",
      aModel.get(), aRevision.get(), aFile.get());

  if (!CanSend()) {
    return ModelFilePathPromise::CreateAndReject(
        "MLModelHubChild not connected"_ns, __func__);
  }

  return SendGetModelFilePath(aModel, aRevision, aFile)
      ->Then(GetCurrentSerialEventTarget(), __func__,
             [](const nsCString& aFilePath) {
               return ModelFilePathPromise::CreateAndResolve(aFilePath, __func__);
             },
             [](ResponseRejectReason aReason) {
               return ModelFilePathPromise::CreateAndReject(
                   "IPC failed"_ns, __func__);
             });
}

RefPtr<ModelBlobPromise> MLModelHubChild::GetModelBlob(
    const nsCString& aModel, const nsCString& aRevision, const nsCString& aFile) {
  LOG(LogLevel::Debug, "GetModelBlob: model=%s revision=%s file=%s",
      aModel.get(), aRevision.get(), aFile.get());

  if (!CanSend()) {
    LOG(LogLevel::Error, "GetModelBlob: MLModelHubChild not connected!");
    return ModelBlobPromise::CreateAndReject(
        "MLModelHubChild not connected"_ns, __func__);
  }

  LOG(LogLevel::Debug, "GetModelBlob: Sending IPC request to parent");
  auto ipcPromise = SendGetModelBlob(aModel, aRevision, aFile);
  if (!ipcPromise) {
    LOG(LogLevel::Error, "GetModelBlob: Failed to send IPC request!");
    return ModelBlobPromise::CreateAndReject(
        "Failed to send IPC request"_ns, __func__);
  }

  LOG(LogLevel::Debug, "GetModelBlob: IPC request sent, setting up promise handlers");
  return ipcPromise->Then(GetCurrentSerialEventTarget(), __func__,
             [aModel, aRevision, aFile](const ModelBlobResult& aResult) {
               LOG(LogLevel::Debug, "GetModelBlob: Received IPC response for model=%s, success=%d",
                   aModel.get(), aResult.success());

               if (aResult.success()) {
                 if (aResult.blob()) {
                   LOG(LogLevel::Debug, "GetModelBlob: Successfully got blob data");
                   return ModelBlobPromise::CreateAndResolve(aResult.blob().ref(), __func__);
                 } else {
                   LOG(LogLevel::Error, "GetModelBlob: Success=true but no blob data!");
                   return ModelBlobPromise::CreateAndReject(
                       "No blob data in successful response"_ns, __func__);
                 }
               } else {
                 LOG(LogLevel::Error, "GetModelBlob: Failed with error: %s", aResult.error().get());
                 return ModelBlobPromise::CreateAndReject(
                     aResult.error(), __func__);
               }
             },
             [aModel](ResponseRejectReason aReason) {
               LOG(LogLevel::Error, "GetModelBlob: IPC failed for model=%s, reason=%d",
                   aModel.get(), static_cast<int>(aReason));
               return ModelBlobPromise::CreateAndReject(
                   "IPC failed"_ns, __func__);
             });
}

mozilla::ipc::IPCResult MLModelHubChild::RecvOnModelDownloadProgress(
    nsCString&& aSessionId, ModelDownloadProgress&& aProgress) {
  LOG(LogLevel::Debug, "RecvOnModelDownloadProgress: session=%s progress=%d",
      aSessionId.get(), aProgress.progress());

  auto pendingDownload = mPendingDownloads.Lookup(aSessionId);
  if (pendingDownload && (*pendingDownload)->mProgressCallback) {
    (*pendingDownload)->mProgressCallback->OnProgress(
        aProgress.progress(), aProgress.currentLoaded(),
        aProgress.totalLoaded(), aProgress.total());
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult MLModelHubChild::RecvOnModelDownloadComplete(
    nsCString&& aSessionId, ModelDownloadResult&& aResult) {
  LOG(LogLevel::Debug, "RecvOnModelDownloadComplete: session=%s success=%d",
      aSessionId.get(), aResult.success());

  auto pendingDownload = mPendingDownloads.Extract(aSessionId);
  if (pendingDownload.isSome() && *pendingDownload) {
    if (aResult.success()) {
      (*pendingDownload)->mPromise->Resolve(true, __func__);
    } else {
      (*pendingDownload)->mPromise->Reject(aResult.error(), __func__);
    }
  }

  return IPC_OK();
}

void MLModelHubChild::ActorDestroy(ActorDestroyReason aReason) {
  LOG(LogLevel::Debug, "MLModelHubChild::ActorDestroy reason=%d",
      static_cast<int>(aReason));

  // Reject all pending downloads
  for (auto& entry : mPendingDownloads) {
    if (entry.GetData()->mPromise) {
      entry.GetData()->mPromise->Reject("Actor destroyed"_ns, __func__);
    }
  }
  mPendingDownloads.Clear();
}

// MLModelHubService implementation
static StaticRefPtr<MLModelHubService> sMLModelHubServiceInstance;
static StaticMutex sMLModelHubServiceMutex;

/* static */
MLModelHubService& MLModelHubService::GetSingleton() {
  StaticMutexAutoLock lock(sMLModelHubServiceMutex);
  if (!sMLModelHubServiceInstance) {
    LOG(LogLevel::Debug, "MLModelHubService::GetSingleton: Creating new instance");
    sMLModelHubServiceInstance = new MLModelHubService();
    ClearOnShutdown(&sMLModelHubServiceInstance);
  } else {
    LOG(LogLevel::Debug, "MLModelHubService::GetSingleton: Returning existing instance");
  }
  return *sMLModelHubServiceInstance;
}

bool MLModelHubService::Initialize(ipc::Endpoint<PMLModelHubChild>&& aEndpoint) {
  LOG(LogLevel::Debug, "MLModelHubService::Initialize called");
  MutexAutoLock lock(mMutex);

  if (mChild) {
    LOG(LogLevel::Warning, "MLModelHubService already initialized");
    return false;
  }

  LOG(LogLevel::Debug, "MLModelHubService::Initialize: Creating MLModelHubChild");
  mChild = MLModelHubChild::Create();
  if (!mChild) {
    LOG(LogLevel::Error, "MLModelHubService::Initialize: Failed to create MLModelHubChild");
    return false;
  }

  LOG(LogLevel::Debug, "MLModelHubService::Initialize: Binding endpoint");
  if (!aEndpoint.Bind(mChild)) {
    LOG(LogLevel::Error, "Failed to bind MLModelHubChild endpoint");
    mChild = nullptr;
    return false;
  }

  LOG(LogLevel::Debug, "MLModelHubService initialized successfully");
  return true;
}

RefPtr<ModelAvailabilityPromise> MLModelHubService::IsModelAvailable(
    const nsCString& aModel, const nsCString& aRevision, const nsCString& aFilename) {
  MutexAutoLock lock(mMutex);

  if (!mChild) {
    return ModelAvailabilityPromise::CreateAndReject(
        "MLModelHubService not initialized"_ns, __func__);
  }

  return mChild->IsModelAvailable(aModel, aRevision, aFilename);
}

RefPtr<ModelDownloadPromise> MLModelHubService::DownloadModel(
    const nsCString& aTaskName, const nsCString& aModel, const nsCString& aRevision,
    const nsTArray<nsCString>& aFiles, ModelDownloadProgressCallback* aProgressCallback) {
  MutexAutoLock lock(mMutex);

  if (!mChild) {
    return ModelDownloadPromise::CreateAndReject(
        "MLModelHubService not initialized"_ns, __func__);
  }

  return mChild->DownloadModel(aTaskName, aModel, aRevision, aFiles, aProgressCallback);
}

RefPtr<ModelFilePathPromise> MLModelHubService::GetModelFilePath(
    const nsCString& aModel, const nsCString& aRevision, const nsCString& aFile) {
  MutexAutoLock lock(mMutex);

  if (!mChild) {
    return ModelFilePathPromise::CreateAndReject(
        "MLModelHubService not initialized"_ns, __func__);
  }

  return mChild->GetModelFilePath(aModel, aRevision, aFile);
}

RefPtr<ModelBlobPromise> MLModelHubService::GetModelBlob(
    const nsCString& aModel, const nsCString& aRevision, const nsCString& aFile) {
  LOG(LogLevel::Debug, "MLModelHubService::GetModelBlob: model=%s revision=%s file=%s",
      aModel.get(), aRevision.get(), aFile.get());

  MutexAutoLock lock(mMutex);

  if (!mChild) {
    LOG(LogLevel::Error, "MLModelHubService::GetModelBlob: mChild is null - service not initialized!");
    return ModelBlobPromise::CreateAndReject(
        "MLModelHubService not initialized"_ns, __func__);
  }

  LOG(LogLevel::Debug, "MLModelHubService::GetModelBlob: Calling mChild->GetModelBlob");
  return mChild->GetModelBlob(aModel, aRevision, aFile);
}

RefPtr<MLModelHubChild> MLModelHubService::GetChild() {
  MutexAutoLock lock(mMutex);
  return mChild;
}

void MLModelHubService::Shutdown() {
  MutexAutoLock lock(mMutex);
  mChild = nullptr;
  LOG(LogLevel::Debug, "MLModelHubService shutdown complete");
}

} // namespace ml
} // namespace mozilla
