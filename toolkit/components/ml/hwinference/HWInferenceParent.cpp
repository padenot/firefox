/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/StaticPtr.h"
#include "HWInferenceParent.h"
#include "HWInferenceManagerParent.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/UtilityProcessManager.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/FileBlobImpl.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/Blob.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/ErrorResult.h"
#include "nsIMLModelHub.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsLocalFile.h"
#include "mozilla/Logging.h"
#include <functional>

namespace mozilla::ipc {

extern LazyLogModule gHWInferenceLog;
#define LOGE(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, __VA_ARGS__)
#define LOGD(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, __VA_ARGS__)
#define LOGV(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Verbose, __VA_ARGS__)

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

class ModelDownloadProgressCallback final
    : public nsIMLModelDownloadProgressCallback {
 public:
  NS_DECL_ISUPPORTS

  explicit ModelDownloadProgressCallback(const nsCString& aModel)
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

static StaticRefPtr<HWInferenceParent> sSingleton;

void HWInferenceParent::ActorDestroy(ActorDestroyReason aReason) {
  sSingleton = nullptr;
}

/* static */
RefPtr<HWInferenceParent> HWInferenceParent::GetSingleton() {
  if (!sSingleton) {
    sSingleton = new HWInferenceParent();
  }
  return sSingleton;
}

nsresult HWInferenceParent::BindToUtilityProcess(
    const RefPtr<UtilityProcessParent>& aUtilityParent) {
  LOGD("{}", __func__);
  Endpoint<PHWInferenceParent> parentEnd;
  Endpoint<PHWInferenceChild> childEnd;
  nsresult rv = PHWInference::CreateEndpoints(
      EndpointProcInfo::Current(), aUtilityParent->OtherEndpointProcInfo(),
      &parentEnd, &childEnd);

  if (NS_FAILED(rv)) {
    LOGE("Failed to create PHWInference endpoints: {:x}",
         static_cast<uint32_t>(rv));
    MOZ_ASSERT(false, "Protocol endpoints failure");
    return NS_ERROR_FAILURE;
  }

  LOGD("Sending StartHWInferenceService to utility process");
  if (!aUtilityParent->SendStartHWInferenceService(std::move(childEnd))) {
    LOGE("Failed to send StartHWInferenceService");
    MOZ_ASSERT(false, "StartHWInference service failure");
    return NS_ERROR_FAILURE;
  }

  LOGD("StartHWInferenceService sent successfully, binding parent endpoint");
  DebugOnly<bool> ok = parentEnd.Bind(this);
  MOZ_ASSERT(ok);
  return NS_OK;
}

mozilla::ipc::IPCResult HWInferenceParent::RecvIsModelAvailable(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    IsModelAvailableResolver&& aResolver) {
  LOGD("{}: model={} revision={} filename={}", __func__, aModel.get(),
       aRevision.get(), aFilename.get());

  // ModelHub is the module that handles model management, and is implemented in
  // JavaScript. We call into it using a thin XPCOM layer, and XPCOM is main
  // thread only.
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "HWInferenceParent::RecvIsModelAvailable",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), filename = std::move(aFilename),
       resolver = std::move(aResolver), &aResolver]() mutable {
        nsCOMPtr<nsIMLModelHub> modelHubService =
            do_GetService("@mozilla.org/ml-modelhub;1");

        if (!modelHubService) {
          LOGE("{} - Failed to get ModelHub XPCOM service", __func__);
          resolver(false);
          return;
        }

        RefPtr<dom::Promise> promise;
        nsresult rv = modelHubService->IsModelAvailable(
            NS_ConvertUTF8toUTF16(model), NS_ConvertUTF8toUTF16(revision),
            NS_ConvertUTF8toUTF16(filename), getter_AddRefs(promise));

        if (NS_FAILED(rv) || !promise) {
          LOGE("{}  ERROR: ModelHub call failed with nsresult={:x}", __func__,
               static_cast<uint32_t>(rv));
          resolver(false);
          return;
        }

        promise->AppendNativeHandler(new PromiseHandler(
            [aResolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              bool available = aValue.toBoolean();
              LOGD("{} Promise resolved, available={}", __func__,
                   available ? "true" : "false");
              aResolver(available);
            },
            [aResolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              LOGE("{} - ERROR: Promise rejected", __func__);
              aResolver(false);
            }));
      }));

  return IPC_OK();
}

IPCResult HWInferenceParent::RecvInstallModel(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    InstallModelResolver&& aResolver) {
  LOGD("{} model=%s revision=%s filename=%s", __func__, aModel.get(),
       aRevision.get(), aFilename.get());

  // ModelHub is the module that handles model management, and is implemented in
  // JavaScript. We call into it using a thin XPCOM layer, and XPCOM is main
  // thread only.
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "HWInferenceParent::RecvInstallModel",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), filename = std::move(aFilename),
       resolver = std::move(aResolver)]() mutable {
        nsCOMPtr<nsIMLModelHub> modelHubService =
            do_GetService("@mozilla.org/ml-modelhub;1");

        if (!modelHubService) {
          LOGE("{} - Failed to get ModelHub XPCOM service", __func__);
          resolver(false);
          return;
        }

        nsTArray<nsString> files;
        files.AppendElement(NS_ConvertUTF8toUTF16(filename));

        RefPtr<ModelDownloadProgressCallback> progressCallback =
            new ModelDownloadProgressCallback(model);
        RefPtr<ModelDownloadCompletionCallback> completionCallback =
            new ModelDownloadCompletionCallback(std::move(resolver));

        nsString downloadSessionId;
        nsresult rv = modelHubService->DownloadModel(
            u"speech-recognition"_ns, NS_ConvertUTF8toUTF16(model),
            NS_ConvertUTF8toUTF16(revision), files, progressCallback,
            completionCallback, downloadSessionId);

        // The completion callback will call the resolver, both in the error and
        // success cases.
        if (NS_FAILED(rv) || downloadSessionId.IsEmpty()) {
          LOGE(
              "{} - ERROR: ModelHub DownloadModel call failed with "
              "nsresult={:x}",
              __func__, static_cast<uint32_t>(rv));
          completionCallback->OnError(u"Failed to start download"_ns);
          return;
        }
        LOGD("{} download started successfully with session ID: {}", __func__,
             NS_ConvertUTF16toUTF8(downloadSessionId).get());
      }));

  return IPC_OK();
}

mozilla::ipc::IPCResult HWInferenceParent::RecvGetModelBlob(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    GetModelBlobResolver&& aResolver) {
  LOGD("{}, dispatching to main thread", __func__);

  // ModelHub is the module that handles model management, and is implemented in
  // JavaScript. We call into it using a thin XPCOM layer, and XPCOM is main
  // thread only.
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "HWInferenceParent::RecvGetModelBlob",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), filename = std::move(aFilename),
       resolver = std::move(aResolver)]() mutable {
        nsCOMPtr<nsIMLModelHub> modelHubService =
            do_GetService("@mozilla.org/ml-modelhub;1");

        if (!modelHubService) {
          LOGE("{} - ERROR: Failed to get ModelHub XPCOM service", __func__);
          GetModelBlobError error;
          error.errorCode() = NS_ERROR_FAILURE;
          resolver(GetModelBlobResult(error));
          return;
        }

        RefPtr<dom::Promise> promise;
        nsresult rv = modelHubService->GetModelBlob(
            NS_ConvertUTF8toUTF16(model), NS_ConvertUTF8toUTF16(revision),
            NS_ConvertUTF8toUTF16(filename), getter_AddRefs(promise));

        if (NS_FAILED(rv)) {
          LOGE("{} - ERROR: GetModelBlob call failed with rv={:x}", __func__,
               static_cast<uint32_t>(rv));
          GetModelBlobError error;
          error.errorCode() = rv;
          resolver(GetModelBlobResult(error));
          return;
        }

        MOZ_ASSERT(promise);

        promise->AppendNativeHandler(new PromiseHandler(
            [resolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              // This comes from chrome js, we can assert
              MOZ_ASSERT(aValue.isObject());

              // Extract the Blob and serialize it for IPC
              RefPtr<dom::Blob> blob;
              nsresult rv = UNWRAP_OBJECT(Blob, &aValue.toObject(), blob);
              MOZ_ASSERT(NS_SUCCESS(rv));

              RefPtr<dom::BlobImpl> blobImpl = blob->Impl();
              dom::IPCBlob ipcBlob;
              rv = dom::IPCBlobUtils::Serialize(blobImpl, ipcBlob);
              if (NS_FAILED(rv)) {
                LOGE("ERROR: Failed to serialize blob");
                GetModelBlobError error;
                error.errorCode() = rv;
                resolver(GetModelBlobResult(error));
                return;
              }

              LOGE("Successfully retrieved and serialized blob for model file");
              GetModelBlobSuccess success;
              success.blob() = ipcBlob;
              resolver(GetModelBlobResult(success));
            },
            [resolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              LOGE("{} - ERROR: promise rejected", __func__);

              if (aValue.isObject()) {
                JS::Rooted<JSObject*> obj(aCx, &aValue.toObject());
                JS::Rooted<JS::Value> msgVal(aCx);
                if (JS_GetProperty(aCx, obj, "message", &msgVal) &&
                    msgVal.isString()) {
                  JS::Rooted<JSString*> str(aCx, msgVal.toString());
                  nsAutoJSString autoStr;
                  if (autoStr.init(aCx, str)) {
                    LOGE("{} - Rejection message: {}", __func__,
                         NS_ConvertUTF16toUTF8(autoStr).get());
                  }
                }
              }

              GetModelBlobError error;
              error.errorCode() = NS_ERROR_FAILURE;
              resolver(GetModelBlobResult(error));
            }));
      }));

  return IPC_OK();
}

}  // namespace mozilla::ipc

#undef LOGD
#undef LOGV
