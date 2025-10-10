/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/StaticPtr.h"
#include "HWInferenceParent.h"
#include "HWInferenceManagerParent.h"
#include "mozilla/dom/Blob.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/UtilityProcessManager.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/Promise-inl.h"
#include "mozilla/dom/FileBlobImpl.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/Blob.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/ErrorResult.h"
#include "nsString.h"
#include "mozilla/Logging.h"
#include "nsIMLModelHub.h"
#include <functional>

namespace mozilla::ipc {

extern LazyLogModule gHWInferenceLog;
#define LOGE(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, __VA_ARGS__)
#define LOGD(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, __VA_ARGS__)
#define LOGV(...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Verbose, __VA_ARGS__)

StaticRefPtr<HWInferenceParent> HWInferenceParent::sSingleton;

/* static */
RefPtr<HWInferenceParent> HWInferenceParent::GetSingleton() {
  MOZ_ASSERT(AssertIsOnMainThread());
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
    const RefPtr<UtilityProcessParent>& aUtilityParent) {
  LOGD("{}", __func__);
  Endpoint<PHWInferenceParent> parentEnd;
  Endpoint<PHWInferenceChild> childEnd;
  MOZ_ALWAYS_SUCCEEDS(PHWInference::CreateEndpoints(
      EndpointProcInfo::Current(), aUtilityParent->OtherEndpointProcInfo(),
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
    nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
    IsModelAvailableResolver&& aResolver) {
  LOGD("{}: model={} revision={} filename={}", __func__, aModel.get(),
       aRevision.get(), aFilename.get());

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
  nsresult rv = modelHubService->IsModelAvailable(aModel, aRevision, aFilename,
                                                  getter_AddRefs(promise));

  if (NS_FAILED(rv) || !promise) {
    LOGE("{}  ERROR: ModelHub call failed with nsresult={:x}", __func__,
         static_cast<uint32_t>(rv));
    aResolver(false);
    return IPC_OK();
  }

  (void)promise->ThenCatchWithCycleCollectedArgs(
      [aResolver](
          JSContext* aCx, JS::Handle<JS::Value> aArg,
          ErrorResult& aRv) -> already_AddRefed<dom::Promise> {
        aResolver(JS::ToBoolean(aArg));
        return nullptr;
      },
      [aResolver](
          JSContext* aCx, JS::Handle<JS::Value> aArg,
          ErrorResult& aRv) -> already_AddRefed<dom::Promise> {
        aResolver(false);
        return nullptr;
      });

  return IPC_OK();
}

}  // namespace mozilla::ipc

#undef LOGD
#undef LOGV
#undef LOGE
