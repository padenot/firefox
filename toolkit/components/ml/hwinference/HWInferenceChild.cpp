/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceChild.h"
#include "HWInferenceManagerParent.h"
#include "mozilla/Logging.h"
#include "nsDebugImpl.h"

namespace mozilla::hwinference {

LazyLogModule gHWInferenceLog("HWInference");
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

HWInferenceChild::HWInferenceChild() {
  nsDebugImpl::SetMultiprocessMode("HWInference");
}

void HWInferenceChild::Shutdown() { PHWInferenceChild::Close(); }

ipc::IPCResult HWInferenceChild::RecvNewContentHWInferenceManager(
    Endpoint<hwinference::PHWInferenceManagerParent>&& aEndpoint,
    const dom::ContentParentId& aContentId) {
  LOGD("[{} - {}] Received connection request from content {}", fmt::ptr(this),
       __func__, static_cast<uint64_t>(aContentId));

  if (!HWInferenceManagerParent::CreateForContent(std::move(aEndpoint),
                                                  aContentId)) {
    LOGE(
        "[{} - {}]"
        "Error: Failed to create HWInferenceManagerParent, content id: {}",
        fmt::ptr(this), __func__, static_cast<uint64_t>(aContentId));
    return IPC_FAIL(this, "Failed to create HWInferenceManagerParent");
  }

  LOGD("[{} - {}] Successfully created HWInferenceManagerParent for content {}",
       fmt::ptr(this), __func__, static_cast<uint64_t>(aContentId));
  return IPC_OK();
}

RefPtr<HWInferenceChild::IsModelAvailablePromise>
HWInferenceChild::SendIsModelAvailable(const nsCString& aEngine,
                                       const nsCString& aModel,
                                       const nsCString& aRevision,
                                       const nsCString& aFilename) {
  LOGD(
      "[{} - {}] Sending model availability request to parent process: "
      "engine={} model={} revision={} filename={}",
      fmt::ptr(this), __func__, aEngine.get(), aModel.get(), aRevision.get(),
      aFilename.get());

  return PHWInferenceChild::SendIsModelAvailable(aEngine, aModel, aRevision,
                                                 aFilename);
}

RefPtr<HWInferenceChild::InstallModelPromise>
HWInferenceChild::SendInstallModel(const nsCString& aTask,
                                   const nsCString& aModel,
                                   const nsCString& aRevision,
                                   const nsCString& aFilename) {
  LOGD(
      "[{} - {}] Sending model installation request to parent process: "
      "task={} model={} revision={} filename={}",
      fmt::ptr(this), __func__, aTask.get(), aModel.get(), aRevision.get(),
      aFilename.get());

  return PHWInferenceChild::SendInstallModel(aTask, aModel, aRevision,
                                             aFilename);
}

RefPtr<HWInferenceChild::GetModelFilePromise>
HWInferenceChild::SendGetModelFile(const nsACString& aEngineId,
                                   const nsACString& aTask,
                                   const nsACString& aModel,
                                   const nsACString& aRevision,
                                   const nsACString& aFilename) {
  LOGD(
      "[{} - {}] Sending model file request to parent process: engineId={} "
      "task={} model={} revision={} filename={}",
      fmt::ptr(this), __func__, aEngineId, aTask, aModel,
      aRevision, aFilename);

  return PHWInferenceChild::SendGetModelFile(aEngineId, aTask, aModel,
                                             aRevision, aFilename);
}

}  // namespace mozilla::hwinference

#undef LOG
