/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/StaticPtr.h"
#include "HWInferenceParent.h"
#include "HWInferenceManagerParent.h"
#include "mozilla/dom/Blob.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/UtilityProcessManager.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/Promise-inl.h"
#include "nsIFileStreams.h"
#include "nsIInputStream.h"
#include "mozilla/ErrorResult.h"
#include "nsString.h"
#include "mozilla/Logging.h"
#include "nsIMLModelHub.h"
#include "prio.h"
#include "private/pprio.h"

#ifdef XP_WIN
#  include <windows.h>
#endif


namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGE(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, __VA_ARGS__)
#define LOGD(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, __VA_ARGS__)
#define LOGV(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Verbose, __VA_ARGS__)

StaticRefPtr<HWInferenceParent> HWInferenceParent::sSingleton;

class ModelDownloadProgressCallback final
    : public nsIMLModelDownloadProgressCallback {
 public:
  NS_DECL_ISUPPORTS

  explicit ModelDownloadProgressCallback(nsACString& aModel)
      : mModel(aModel) {}

  NS_IMETHOD OnProgress(int32_t aProgress, int64_t aCurrentLoaded,
                        int64_t aTotalLoaded, int64_t aTotal) override {
    LOGV("{} - model={} progress={}% current={} total loaded={} total={}",
         __func__, mModel.get(), aProgress, aCurrentLoaded, aTotalLoaded,
         aTotal);
    return NS_OK;
  }

 private:
  ~ModelDownloadProgressCallback() = default;
  nsCString mModel;
};

NS_IMPL_ISUPPORTS(ModelDownloadProgressCallback,
                  nsIMLModelDownloadProgressCallback)

class ModelDownloadCompletionCallback final
    : public nsIMLModelDownloadCompletionCallback {
 public:
  NS_DECL_ISUPPORTS

  explicit ModelDownloadCompletionCallback(
      HWInferenceParent::InstallModelResolver&& aResolver)
      : mResolver(std::move(aResolver)) {}

  NS_IMETHOD OnSuccess(const nsAString& aModel,
                       const nsAString& aRevision) override {
    LOGD("{} - model={} revision={}", __func__,
         NS_ConvertUTF16toUTF8(aModel).get(),
         NS_ConvertUTF16toUTF8(aRevision).get());
    mResolver(true);
    return NS_OK;
  }

  NS_IMETHOD OnError(const nsAString& aError) override {
    LOGE("{} - Error when downloading {}", __func__,
         NS_ConvertUTF16toUTF8(aError).get());
    mResolver(false);
    return NS_OK;
  }

 private:
  ~ModelDownloadCompletionCallback() = default;
  HWInferenceParent::InstallModelResolver mResolver;
};

NS_IMPL_ISUPPORTS(ModelDownloadCompletionCallback,
                  nsIMLModelDownloadCompletionCallback)

static nsresult BlobJSObjectToFileDescriptor(JSContext* aCx,
                                             JS::Handle<JS::Value> aValue,
                                             ipc::FileDescriptor* aDesc) {
  if (!aValue.isObject()) {
    return NS_ERROR_UNEXPECTED;
  }

  RefPtr<dom::Blob> blob;
  nsresult rv = UNWRAP_OBJECT(Blob, &aValue.toObject(), blob);
  if (NS_FAILED(rv)) {
    LOGE("BlobJSObjectToFileDescriptor - ERROR: Failed to unwrap Blob: {:x}",
         static_cast<uint32_t>(rv));
    return rv;
  }

  ErrorResult errorResult;
  nsCOMPtr<nsIInputStream> stream;
  blob->CreateInputStream(getter_AddRefs(stream), errorResult);
  if (errorResult.Failed()) {
    LOGE(
        "BlobJSObjectToFileDescriptor - ERROR: Failed to create input stream "
        "from blob");
    return NS_ERROR_UNEXPECTED;
  }

  nsCOMPtr<nsIFileMetadata> fileMetadata = do_QueryInterface(stream);
  if (!fileMetadata) {
    LOGE(
        "BlobJSObjectToFileDescriptor - ERROR: Stream doesn't support "
        "nsIFileMetadata");
    return NS_ERROR_UNEXPECTED;
  }

  PRFileDesc* fileDesc;
  nsresult getRv = fileMetadata->GetFileDescriptor(&fileDesc);
  if (NS_FAILED(getRv)) {
    LOGE("BlobJSObjectToFileDescriptor - ERROR: GetFileDescriptor failed: {:x}",
         static_cast<uint32_t>(getRv));
    return getRv;
  }

  ipc::FileDescriptor fd(
      ipc::FileDescriptor::PlatformHandleType(PR_FileDesc2NativeHandle(fileDesc)));
  if (!fd.IsValid()) {
    LOGE("BlobJSObjectToFileDescriptor - ERROR: Failed to get native handle");
    return NS_ERROR_UNEXPECTED;
  }

  *aDesc = std::move(fd);
  return NS_OK;
}

/* static */
RefPtr<HWInferenceParent> HWInferenceParent::GetSingleton() {
  AssertIsOnMainThread();
  if (!sSingleton) {
    sSingleton = new HWInferenceParent();
  }
  return sSingleton;
}

void HWInferenceParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("{}", __func__);
  sSingleton = nullptr;
}

nsresult HWInferenceParent::BindToUtilityProcess(
    const RefPtr<ipc::UtilityProcessParent>& aUtilityParent) {
  LOGD("{}", __func__);
  Endpoint<hwinference::PHWInferenceParent> parentEnd;
  Endpoint<hwinference::PHWInferenceChild> childEnd;
  MOZ_ALWAYS_SUCCEEDS(PHWInference::CreateEndpoints(
      ipc::EndpointProcInfo::Current(), aUtilityParent->OtherEndpointProcInfo(),
      &parentEnd, &childEnd));

  LOGD("Sending StartHWInferenceService to utility process");
  if (!aUtilityParent->SendStartHWInferenceService(std::move(childEnd))) {
    LOGE("Failed to send StartHWInferenceService");
    MOZ_ASSERT(false, "StartHWInference service failure");
    return NS_ERROR_FAILURE;
  }

  LOGD("StartHWInferenceService sent successfully, binding parent endpoint");
  MOZ_ALWAYS_TRUE(parentEnd.Bind(this));
  return NS_OK;
}

mozilla::ipc::IPCResult HWInferenceParent::RecvIsModelAvailable(
    nsCString&& aEngine, nsCString&& aModel, nsCString&& aRevision,
    nsCString&& aFilename, IsModelAvailableResolver&& aResolver) {
  LOGD("{}: engine={} model={} revision={} filename={}", __func__, aEngine,
       aModel, aRevision, aFilename);

  // ModelHub is the module that handles model management, and is implemented in
  // JavaScript. We're already on the main thread, so we can call into it
  // directly.
  nsCOMPtr<nsIMLModelHub> modelHubService =
      do_GetService("@mozilla.org/ml-modelhub;1");

  if (!modelHubService) {
    LOGE("{} - Failed to get ModelHub XPCOM service", __func__);
    aResolver(false);
    return IPC_OK();
  }

  RefPtr<dom::Promise> promise;
  nsresult rv = modelHubService->IsModelAvailable(
      aEngine, aModel, aRevision, aFilename, getter_AddRefs(promise));

  if (NS_FAILED(rv) || !promise) {
    LOGE("{}  ERROR: ModelHub call failed with nsresult={:x}", __func__,
         static_cast<uint32_t>(rv));
    aResolver(false);
    return IPC_OK();
  }

  (void)promise->AddCallbacksWithCycleCollectedArgs(
      [aResolver](JSContext* aCx, JS::Handle<JS::Value> aArg,
                  ErrorResult& aRv) {
        aResolver(JS::ToBoolean(aArg));
      },
      [aResolver](JSContext* aCx, JS::Handle<JS::Value> aArg,
                  ErrorResult& aRv) {
        aResolver(false);
      });

  return IPC_OK();
}

ipc::IPCResult HWInferenceParent::RecvInstallModel(
    nsCString&& aTask, nsCString&& aModel, nsCString&& aRevision,
    nsCString&& aFilename, InstallModelResolver&& aResolver) {
  LOGD("{} task=%s model=%s revision=%s filename=%s", __func__, aTask,
       aModel, aRevision, aFilename);

  // ModelHub handles model management and is implemented in JavaScript. We're
  // already on the main thread, so we can call it directly.
  nsCOMPtr<nsIMLModelHub> modelHubService =
      do_GetService("@mozilla.org/ml-modelhub;1");

  if (!modelHubService) {
    LOGE("{} - Failed to get ModelHub XPCOM service", __func__);
    aResolver(false);
    return IPC_OK();
  }

  nsTArray<nsCString> files;
  files.AppendElement(aFilename);

  RefPtr<ModelDownloadProgressCallback> progressCallback =
      new ModelDownloadProgressCallback(aModel);
  RefPtr<ModelDownloadCompletionCallback> completionCallback =
      new ModelDownloadCompletionCallback(std::move(aResolver));

  nsString downloadSessionId;
  nsresult rv = modelHubService->DownloadModel(
      aTask, aModel, aRevision, files, progressCallback, completionCallback,
      downloadSessionId);

  // The completion callback will call the resolver, both in the error and
  // success cases.
  if (NS_FAILED(rv) || downloadSessionId.IsEmpty()) {
    LOGE(
        "{} - ERROR: ModelHub DownloadModel call failed with "
        "nsresult={:x}",
        __func__, static_cast<uint32_t>(rv));
    completionCallback->OnError(u"Failed to start download"_ns);
    return IPC_OK();
  }
  LOGD("{} download started successfully with session ID: {}", __func__,
       NS_ConvertUTF16toUTF8(downloadSessionId).get());

  return IPC_OK();
}

ipc::IPCResult HWInferenceParent::RecvGetModelFile(
    nsCString&& aEngineId, nsCString&& aTask, nsCString&& aModel,
    nsCString&& aRevision, nsCString&& aFilename,
    GetModelFileResolver&& aResolver) {
  LOGD("{} engineId={} task={} model={} revision={} filename={}", __func__,
       aEngineId.get(), aTask.get(), aModel.get(), aRevision.get(),
       aFilename.get());

  // ModelHub is the module that handles model management, and is implemented in
  // JavaScript. We're already on the main thread, so we can call into it
  // directly.
  nsCOMPtr<nsIMLModelHub> modelHubService =
      do_GetService("@mozilla.org/ml-modelhub;1");

  if (!modelHubService) {
    LOGE("{} - ERROR: Failed to get ModelHub XPCOM service", __func__);
    GetModelError error;
    error.errorCode() = NS_ERROR_FAILURE;
    aResolver(GetModelFileResult(error));
    return IPC_OK();
  }

  RefPtr<dom::Promise> promise;
  nsresult rv = modelHubService->GetModelBlob(
      aEngineId, aTask, aModel, aRevision, aFilename, getter_AddRefs(promise));

  if (NS_FAILED(rv)) {
    LOGE("{} - ERROR: GetModelBlob call failed with rv={:x}", __func__,
         static_cast<uint32_t>(rv));
    GetModelError error;
    error.errorCode() = rv;
    aResolver(GetModelFileResult(error));
    return IPC_OK();
  }

  promise->AddCallbacksWithCycleCollectedArgs(
      [aResolver](
          JSContext* aCx, JS::Handle<JS::Value> aValue,
          ErrorResult& aRv) {
        GetModelFileSuccess success;
        nsresult rv = BlobJSObjectToFileDescriptor(aCx, aValue, &success.fd());
        if (NS_FAILED(rv)) {
          aResolver(GetModelFileResult(GetModelError(rv)));
          return;
        }
        MOZ_ASSERT(success.fd().IsValid());
        aResolver(GetModelFileResult(std::move(success)));
      },
      [aResolver](
          JSContext* aCx, JS::Handle<JS::Value> aValue,
          ErrorResult& aRv) {
        LOGE("RecvGetModelFile - ERROR: promise rejected in RecvGetModelFile");
        GetModelError error;
        error.errorCode() = NS_ERROR_FAILURE;
        aResolver(GetModelFileResult(error));
      });

  return IPC_OK();
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGV
#undef LOGE
