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


}  // namespace mozilla::ipc

#undef LOGD
#undef LOGV
