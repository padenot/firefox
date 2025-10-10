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
       resolver = std::move(aResolver)]() mutable {
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
            [resolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              bool available = aValue.toBoolean();
              LOGD("{} Promise resolved, available={}", __func__,
                   available ? "true" : "false");
              resolver(available);
            },
            [resolver, self](JSContext* aCx, JS::Handle<JS::Value> aValue) {
              LOGE("{} - ERROR: Promise rejected", __func__);
              resolver(false);
            }));
      }));

  return IPC_OK();
}


}  // namespace mozilla::ipc

#undef LOGD
#undef LOGV
