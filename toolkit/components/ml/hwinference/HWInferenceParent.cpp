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
#define HWINF_LOG(...) MOZ_LOG(gHWInferenceLog, LogLevel::Debug, (__VA_ARGS__))

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

// Download progress callback implementation
class ModelDownloadProgressCallback final : public nsIMLModelDownloadProgressCallback {
 public:
  NS_DECL_ISUPPORTS

  explicit ModelDownloadProgressCallback(const nsCString& aModel, const nsCString& aRevision)
      : mModel(aModel), mRevision(aRevision) {}

  NS_IMETHOD OnProgress(int32_t aProgress, int64_t aCurrentLoaded,
                        int64_t aTotalLoaded, int64_t aTotal) override {
    HWINF_LOG("[%p] ModelDownloadProgressCallback::OnProgress - Model: %s, Progress: %d%%, "
              "Current: %ld, Total loaded: %ld, Total: %ld",
              this, mModel.get(), aProgress, static_cast<long>(aCurrentLoaded),
              static_cast<long>(aTotalLoaded), static_cast<long>(aTotal));
    return NS_OK;
  }

 private:
  ~ModelDownloadProgressCallback() = default;
  nsCString mModel;
  nsCString mRevision;
};

NS_IMPL_ISUPPORTS(ModelDownloadProgressCallback, nsIMLModelDownloadProgressCallback)

// Download completion callback implementation
class ModelDownloadCompletionCallback final : public nsIMLModelDownloadCompletionCallback {
 public:
  NS_DECL_ISUPPORTS

  explicit ModelDownloadCompletionCallback(HWInferenceParent::InstallModelResolver&& aResolver)
      : mResolver(std::move(aResolver)) {}

  NS_IMETHOD OnSuccess(const nsAString& aModel, const nsAString& aRevision) override {
    HWINF_LOG("[%p] ModelDownloadCompletionCallback::OnSuccess - Model: %s, Revision: %s",
              this, NS_ConvertUTF16toUTF8(aModel).get(), NS_ConvertUTF16toUTF8(aRevision).get());
    mResolver(true);
    return NS_OK;
  }

  NS_IMETHOD OnError(const nsAString& aError) override {
    HWINF_LOG("[%p] ModelDownloadCompletionCallback::OnError - Error: %s",
              this, NS_ConvertUTF16toUTF8(aError).get());
    mResolver(false);
    return NS_OK;
  }

 private:
  ~ModelDownloadCompletionCallback() = default;
  HWInferenceParent::InstallModelResolver mResolver;
};

NS_IMPL_ISUPPORTS(ModelDownloadCompletionCallback, nsIMLModelDownloadCompletionCallback)

static StaticRefPtr<HWInferenceParent> sSingleton;

HWInferenceParent::HWInferenceParent() {
  HWINF_LOG("[%p] HWInferenceParent: Constructor called - parent actor created",
            this);
}

HWInferenceParent::~HWInferenceParent() = default;

void HWInferenceParent::ActorDestroy(ActorDestroyReason aReason) {
  sSingleton = nullptr;
}

void HWInferenceParent::Bind(Endpoint<PHWInferenceParent>&& aEndpoint) {
  HWINF_LOG("[%p] HWInferenceParent: Bind called - IPC connection established",
            this);
  DebugOnly<bool> ok = aEndpoint.Bind(this);
  MOZ_ASSERT(ok);
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
  HWINF_LOG("HWInferenceParent::BindToUtilityProcess called");
  Endpoint<PHWInferenceParent> parentEnd;
  Endpoint<PHWInferenceChild> childEnd;
  nsresult rv = PHWInference::CreateEndpoints(
      EndpointProcInfo::Current(), aUtilityParent->OtherEndpointProcInfo(),
      &parentEnd, &childEnd);

  if (NS_FAILED(rv)) {
    HWINF_LOG("Failed to create PHWInference endpoints: %x",
              static_cast<uint32_t>(rv));
    MOZ_ASSERT(false, "Protocol endpoints failure");
    return NS_ERROR_FAILURE;
  }

  HWINF_LOG("Sending StartHWInferenceService to utility process");
  if (!aUtilityParent->SendStartHWInferenceService(std::move(childEnd))) {
    HWINF_LOG("Failed to send StartHWInferenceService");
    MOZ_ASSERT(false, "StartHWInference service failure");
    return NS_ERROR_FAILURE;
  }

  HWINF_LOG(
      "StartHWInferenceService sent successfully, binding parent endpoint");
  Bind(std::move(parentEnd));
  return NS_OK;
}

mozilla::ipc::IPCResult HWInferenceParent::RecvIsModelAvailable(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    IsModelAvailableResolver&& aResolver) {
  HWINF_LOG(
      "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] Received model "
      "availability request from utility: model=%s revision=%s",
      this, aModel.get(), aRevision.get());

  HWINF_LOG(
      "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] Dispatching to "
      "main thread for ModelHub XPCOM service call",
      this);

  // XPCOM is main thread only
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "HWInferenceParent::RecvIsModelAvailable",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), filename = std::move(aFilename),
       resolver = std::move(aResolver)]() mutable {
        nsCOMPtr<nsIMLModelHub> modelHubService =
            do_GetService("@mozilla.org/ml-modelhub;1");

        if (!modelHubService) {
          HWINF_LOG(
              "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] ERROR: "
              "Failed to get ModelHub XPCOM service",
              self.get());
          resolver(false);
          return;
        }

        HWINF_LOG(
            "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] Calling "
            "ModelHub service",
            self.get());

        // Call the XPCOM service (returns Promise)
        RefPtr<dom::Promise> promise;
        nsresult rv = modelHubService->IsModelAvailable(
            NS_ConvertUTF8toUTF16(model), NS_ConvertUTF8toUTF16(revision),
            NS_ConvertUTF8toUTF16(filename), getter_AddRefs(promise));

        if (NS_FAILED(rv) || !promise) {
          HWINF_LOG(
              "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] ERROR: "
              "ModelHub call failed with nsresult=%x",
              self.get(), static_cast<uint32_t>(rv));
          resolver(false);
          return;
        }

        HWINF_LOG(
            "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] Got "
            "Promise, setting up resolution handler",
            self.get());

        // Since both success and error callbacks need to call resolver,
        // and the resolver can only be moved once, we need shared ownership
        auto sharedResolver = std::make_shared<IsModelAvailableResolver>(std::move(resolver));

        promise->AppendNativeHandler(new PromiseHandler(
            [sharedResolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              // Success handler
              bool available = false;
              if (aValue.isBoolean()) {
                available = aValue.toBoolean();
              }

              HWINF_LOG(
                  "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] "
                  "Promise resolved, available=%s",
                  self.get(), available ? "true" : "false");
              HWINF_LOG(
                  "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] "
                  "Sending response back to utility process",
                  self.get());

              (*sharedResolver)(available);
            },
            [sharedResolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              // Error handler
              HWINF_LOG(
                  "[%p] HWInferenceParent::RecvIsModelAvailable - [PARENT] "
                  "ERROR: Promise rejected",
                  self.get());
              (*sharedResolver)(false);
            }));
      }));

  return IPC_OK();
}

mozilla::ipc::IPCResult HWInferenceParent::RecvInstallModel(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    InstallModelResolver&& aResolver) {
  HWINF_LOG(
      "[%p] HWInferenceParent::RecvInstallModel - [PARENT] Received model "
      "install request from utility: model=%s revision=%s filename=%s",
      this, aModel.get(), aRevision.get(), aFilename.get());

  HWINF_LOG(
      "[%p] HWInferenceParent::RecvInstallModel - [PARENT] Dispatching to "
      "main thread for ModelHub XPCOM service call",
      this);

  // XPCOM is main thread only
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "HWInferenceParent::RecvInstallModel",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), filename = std::move(aFilename),
       resolver = std::move(aResolver)]() mutable {
        nsCOMPtr<nsIMLModelHub> modelHubService =
            do_GetService("@mozilla.org/ml-modelhub;1");

        if (!modelHubService) {
          HWINF_LOG(
              "[%p] HWInferenceParent::RecvInstallModel - [PARENT] ERROR: "
              "Failed to get ModelHub XPCOM service",
              self.get());
          resolver(false);
          return;
        }

        HWINF_LOG(
            "[%p] HWInferenceParent::RecvInstallModel - [PARENT] Calling "
            "ModelHub service for installation",
            self.get());

        // Call the XPCOM service to download the model with proper callbacks
        nsTArray<nsString> files;
        files.AppendElement(NS_ConvertUTF8toUTF16(filename));

        // Create progress and completion callbacks
        RefPtr<ModelDownloadProgressCallback> progressCallback =
            new ModelDownloadProgressCallback(model, revision);
        RefPtr<ModelDownloadCompletionCallback> completionCallback =
            new ModelDownloadCompletionCallback(std::move(resolver));

        nsString downloadSessionId;
        nsresult rv = modelHubService->DownloadModel(
            u"speech-recognition"_ns, NS_ConvertUTF8toUTF16(model),
            NS_ConvertUTF8toUTF16(revision), files,
            progressCallback, completionCallback,
            downloadSessionId);

        if (NS_FAILED(rv) || downloadSessionId.IsEmpty()) {
          HWINF_LOG(
              "[%p] HWInferenceParent::RecvInstallModel - [PARENT] ERROR: "
              "ModelHub DownloadModel call failed with nsresult=%x",
              self.get(), static_cast<uint32_t>(rv));
          // Since we moved the resolver to the completion callback,
          // we need to call it directly here on error
          completionCallback->OnError(u"Failed to start download"_ns);
          return;
        }

        HWINF_LOG(
            "[%p] HWInferenceParent::RecvInstallModel - [PARENT] "
            "Model download started successfully with session ID: %s",
            self.get(), NS_ConvertUTF16toUTF8(downloadSessionId).get());

        // The resolver will be called by the completion callback when download finishes
      }));

  return IPC_OK();
}

mozilla::ipc::IPCResult HWInferenceParent::RecvGetModelBlob(
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    GetModelBlobResolver&& aResolver) {
  HWINF_LOG(
      "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] Received model "
      "blob request from utility: model=%s revision=%s filename=%s",
      this, aModel.get(), aRevision.get(), aFilename.get());

  HWINF_LOG(
      "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] Dispatching to "
      "main thread for ModelHub XPCOM service call",
      this);

  // XPCOM is main thread only
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "HWInferenceParent::RecvGetModelBlob",
      [self = RefPtr(this), model = std::move(aModel),
       revision = std::move(aRevision), filename = std::move(aFilename),
       resolver = std::move(aResolver)]() mutable {
        nsCOMPtr<nsIMLModelHub> modelHubService =
            do_GetService("@mozilla.org/ml-modelhub;1");

        if (!modelHubService) {
          HWINF_LOG(
              "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] ERROR: "
              "Failed to get ModelHub XPCOM service",
              self.get());
          GetModelBlobError error;
          error.errorCode() = NS_ERROR_FAILURE;
          resolver(GetModelBlobResult(error));
          return;
        }

        HWINF_LOG(
            "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] Calling "
            "ModelHub service to get blob",
            self.get());

        // Get the model blob from the XPCOM service
        RefPtr<dom::Promise> promise;
        nsresult rv = modelHubService->GetModelBlob(
            NS_ConvertUTF8toUTF16(model), NS_ConvertUTF8toUTF16(revision),
            NS_ConvertUTF8toUTF16(filename), getter_AddRefs(promise));

        if (NS_FAILED(rv)) {
          HWINF_LOG(
              "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] ERROR: "
              "GetModelBlob call failed with rv=0x%x",
              self.get(), static_cast<uint32_t>(rv));
          GetModelBlobError error;
          error.errorCode() = rv;
          resolver(GetModelBlobResult(error));
          return;
        }

        if (!promise) {
          HWINF_LOG(
              "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] ERROR: "
              "GetModelBlob returned null promise",
              self.get());
          GetModelBlobError error;
          error.errorCode() = NS_ERROR_FAILURE;
          resolver(GetModelBlobResult(error));
          return;
        }

        HWINF_LOG(
            "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] Promise "
            "obtained successfully, attaching native handler",
            self.get());

        // Since resolver can only be moved once, we need shared ownership
        auto sharedResolver = std::make_shared<GetModelBlobResolver>(std::move(resolver));

        promise->AppendNativeHandler(new PromiseHandler(
            [sharedResolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              HWINF_LOG(
                  "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] "
                  "Promise resolved, processing blob",
                  self.get());
              // Success handler - we should have a Blob
              if (!aValue.isObject()) {
                HWINF_LOG(
                    "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] ERROR: "
                    "Promise resolved but value is not an object",
                    self.get());
                GetModelBlobError error;
                error.errorCode() = NS_ERROR_UNEXPECTED;
                (*sharedResolver)(GetModelBlobResult(error));
                return;
              }

              // Extract the Blob and serialize it for IPC
              RefPtr<dom::Blob> blob;
              nsresult rv = UNWRAP_OBJECT(Blob, &aValue.toObject(), blob);
              if (NS_FAILED(rv) || !blob) {
                HWINF_LOG(
                    "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] ERROR: "
                    "Failed to unwrap Blob object",
                    self.get());
                GetModelBlobError error;
                error.errorCode() = NS_ERROR_UNEXPECTED;
                (*sharedResolver)(GetModelBlobResult(error));
                return;
              }

              RefPtr<dom::BlobImpl> blobImpl = blob->Impl();
              dom::IPCBlob ipcBlob;
              rv = dom::IPCBlobUtils::Serialize(blobImpl, ipcBlob);
              if (NS_FAILED(rv)) {
                HWINF_LOG(
                    "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] ERROR: "
                    "Failed to serialize blob",
                    self.get());
                GetModelBlobError error;
                error.errorCode() = rv;
                (*sharedResolver)(GetModelBlobResult(error));
                return;
              }

              HWINF_LOG(
                  "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] "
                  "Successfully retrieved and serialized blob for model file",
                  self.get());
              GetModelBlobSuccess success;
              success.blob() = ipcBlob;
              (*sharedResolver)(GetModelBlobResult(success));
            },
            [sharedResolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              // Error handler
              HWINF_LOG(
                  "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] ERROR: "
                  "Promise rejected",
                  self.get());

              // Try to extract error details
              if (aValue.isObject()) {
                JS::Rooted<JSObject*> obj(aCx, &aValue.toObject());
                JS::Rooted<JS::Value> msgVal(aCx);
                if (JS_GetProperty(aCx, obj, "message", &msgVal) && msgVal.isString()) {
                  JS::Rooted<JSString*> str(aCx, msgVal.toString());
                  nsAutoJSString autoStr;
                  if (autoStr.init(aCx, str)) {
                    HWINF_LOG(
                        "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] "
                        "Rejection message: %s",
                        self.get(), NS_ConvertUTF16toUTF8(autoStr).get());
                  }
                }
              }

              GetModelBlobError error;
              error.errorCode() = NS_ERROR_FAILURE;
              (*sharedResolver)(GetModelBlobResult(error));
            }));

        HWINF_LOG(
            "[%p] HWInferenceParent::RecvGetModelBlob - [PARENT] Native handler "
            "attached, waiting for promise resolution",
            self.get());
      }));

  return IPC_OK();
}

}  // namespace mozilla::ipc
