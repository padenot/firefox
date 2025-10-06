/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MLModelHubChild.h"
#include "mozilla/Logging.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/ClearOnShutdown.h"
#include "nsThreadUtils.h"

namespace mozilla::ml {

static LazyLogModule sMLModelHubChildLog("MLModelHubChild");
#define LOGE(...) MOZ_LOG_FMT(sMLModelHubChildLog, LogLevel::Error, __VA_ARGS__)
#define LOGD(...) MOZ_LOG_FMT(sMLModelHubChildLog, LogLevel::Debug, __VA_ARGS__)

RefPtr<ModelAvailabilityPromise> MLModelHubChild::IsModelAvailable(
    const nsCString& aModel, const nsCString& aRevision,
    const nsCString& aFilename) {
  LOGD("IsModelAvailable: model={} revision={} filename={}", aModel.get(),
       aRevision.get(), aFilename.get());

  if (!CanSend()) {
    LOGE("MLModelHubChild not connected");
    return ModelAvailabilityPromise::CreateAndReject(
        "MLModelHubChild not connected"_ns, __func__);
  }

  return SendIsModelAvailable(aModel, aRevision, aFilename)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [](const ModelAvailabilityResult& aResult) {
            return ModelAvailabilityPromise::CreateAndResolve(aResult.available(), __func__);
          },
          [](ResponseRejectReason aReason) {
            return ModelAvailabilityPromise::CreateAndReject("IPC failed"_ns,
                                                             __func__);
          });
}

RefPtr<ModelDownloadPromise> MLModelHubChild::DownloadModel(
    const nsCString& aTaskName, const nsCString& aModel,
    const nsCString& aRevision, const nsTArray<nsCString>& aFiles,
    ModelDownloadProgressCallback* aProgressCallback) {
  LOGD("DownloadModel: model=%s revision=%s files=%zu", aModel.get(),
       aRevision.get(), aFiles.Length());

  if (!CanSend()) {
    return ModelDownloadPromise::CreateAndReject(
        "MLModelHubChild not connected"_ns, __func__);
  }

  // TODO make this nicer
  RefPtr<ModelDownloadPromise::Private> promise =
      new ModelDownloadPromise::Private(__func__);

  SendStartModelDownload(aTaskName, aModel, aRevision,
                         nsTArray<nsCString>(aFiles.Clone()))
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}, promise,
           aProgressCallback](const nsCString& aSessionId) {
            if (aSessionId.IsEmpty()) {
              promise->Reject("Failed to start download"_ns, __func__);
            } else {
              // Store the pending download request
              self->mPendingDownloads.InsertOrUpdate(
                  aSessionId,
                  MakeUnique<PendingDownload>(promise, aProgressCallback));
              LOGD("Started download with session ID: %s", aSessionId.get());
            }
          },
          [promise](ResponseRejectReason aReason) {
            promise->Reject("IPC failed"_ns, __func__);
          });

  return promise;
}

RefPtr<ModelBlobPromise> MLModelHubChild::GetModelBlob(
    const nsCString& aModel, const nsCString& aRevision,
    const nsCString& aFile) {
  LOGD("GetModelBlob: model=%s revision=%s file=%s", aModel.get(),
       aRevision.get(), aFile.get());

  if (!CanSend()) {
    LOGE("GetModelBlob: MLModelHubChild not connected!");
    return ModelBlobPromise::CreateAndReject("MLModelHubChild not connected"_ns,
                                             __func__);
  }

  LOGD("GetModelBlob: Sending IPC request to parent");
  auto ipcPromise = SendGetModelBlob(aModel, aRevision, aFile);
  if (!ipcPromise) {
    LOGE("GetModelBlob: Failed to send IPC request!");
    return ModelBlobPromise::CreateAndReject("Failed to send IPC request"_ns,
                                             __func__);
  }

  LOGD("GetModelBlob: IPC request sent, setting up promise handlers");
  return ipcPromise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [aModel, aRevision, aFile](const ModelBlobResult& aResult) {
        LOGD("GetModelBlob: Received IPC response for model=%s, success=%d",
             aModel.get(), aResult.success());

        if (aResult.success()) {
          if (aResult.blob()) {
            LOGD("GetModelBlob: Successfully got blob data");
            return ModelBlobPromise::CreateAndResolve(aResult.blob().ref(),
                                                      __func__);
          }
          LOGE("GetModelBlob: Success=true but no blob data!");
          return ModelBlobPromise::CreateAndReject(
              "No blob data in successful response"_ns, __func__);
        }
        LOGE("GetModelBlob: Failed with error: %s", aResult.error().get());
        return ModelBlobPromise::CreateAndReject(aResult.error(), __func__);
      },
      [aModel](ResponseRejectReason aReason) {
        LOGE("GetModelBlob: IPC failed for model=%s, reason=%d", aModel.get(),
             static_cast<int>(aReason));
        return ModelBlobPromise::CreateAndReject("IPC failed"_ns, __func__);
      });
}

mozilla::ipc::IPCResult MLModelHubChild::RecvOnModelDownloadProgress(
    nsCString&& aSessionId, ModelDownloadProgress&& aProgress) {
  LOGD("RecvOnModelDownloadProgress: session=%s progress=%d", aSessionId.get(),
       aProgress.progress());

  auto pendingDownload = mPendingDownloads.Lookup(aSessionId);
  if (pendingDownload && (*pendingDownload)->mProgressCallback) {
    (*pendingDownload)
        ->mProgressCallback->OnProgress(
            aProgress.progress(), aProgress.currentLoaded(),
            aProgress.totalLoaded(), aProgress.total());
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult MLModelHubChild::RecvOnModelDownloadComplete(
    nsCString&& aSessionId, ModelDownloadResult&& aResult) {
  LOGD("RecvOnModelDownloadComplete: session=%s success=%d", aSessionId.get(),
       aResult.success());

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
  LOGD("MLModelHubChild::ActorDestroy reason=%d", static_cast<int>(aReason));

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
    LOGD("MLModelHubService::GetSingleton: Creating new instance");
    sMLModelHubServiceInstance = new MLModelHubService();
    ClearOnShutdown(&sMLModelHubServiceInstance);
  } else {
    LOGD("MLModelHubService::GetSingleton: Returning existing instance");
  }
  return *sMLModelHubServiceInstance;
}

bool MLModelHubService::Initialize(
    ipc::Endpoint<PMLModelHubChild>&& aEndpoint) {
  MutexAutoLock lock(mMutex);

  if (mChild) {
    LOGD("MLModelHubService already initialized");
    return true;
  }

  mChild = new MLModelHubChild();

  if (!aEndpoint.Bind(mChild)) {
    LOGE("Failed to bind MLModelHubChild endpoint");
    mChild = nullptr;
    return false;
  }

  LOGD("MLModelHubService initialized successfully");
  return true;
}

RefPtr<ModelAvailabilityPromise> MLModelHubService::IsModelAvailable(
    const nsCString& aModel, const nsCString& aRevision,
    const nsCString& aFilename) {
  MutexAutoLock lock(mMutex);

  if (!mChild) {
    return ModelAvailabilityPromise::CreateAndReject(
        "MLModelHubService not initialized"_ns, __func__);
  }

  return mChild->IsModelAvailable(aModel, aRevision, aFilename);
}

RefPtr<ModelDownloadPromise> MLModelHubService::DownloadModel(
    const nsCString& aTaskName, const nsCString& aModel,
    const nsCString& aRevision, const nsTArray<nsCString>& aFiles,
    ModelDownloadProgressCallback* aProgressCallback) {
  MutexAutoLock lock(mMutex);

  if (!mChild) {
    return ModelDownloadPromise::CreateAndReject(
        "MLModelHubService not initialized"_ns, __func__);
  }

  return mChild->DownloadModel(aTaskName, aModel, aRevision, aFiles,
                               aProgressCallback);
}

RefPtr<ModelBlobPromise> MLModelHubService::GetModelBlob(
    const nsCString& aModel, const nsCString& aRevision,
    const nsCString& aFile) {
  LOGD("MLModelHubService::GetModelBlob: model=%s revision=%s file=%s",
       aModel.get(), aRevision.get(), aFile.get());

  MutexAutoLock lock(mMutex);

  if (!mChild) {
    LOGE(
        "MLModelHubService::GetModelBlob: mChild is null - service not "
        "initialized!");
    return ModelBlobPromise::CreateAndReject(
        "MLModelHubService not initialized"_ns, __func__);
  }

  LOGD("MLModelHubService::GetModelBlob: Calling mChild->GetModelBlob");
  return mChild->GetModelBlob(aModel, aRevision, aFile);
}

void MLModelHubService::Shutdown() {
  MutexAutoLock lock(mMutex);
  mChild = nullptr;
  LOGD("MLModelHubService shutdown complete");
}

}  // namespace mozilla::ml

#undef LOGE
#undef LOGD
