/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceChild.h"
#include "HWInferenceManagerParent.h"
#include "mozilla/Logging.h"
#include "nsDebugImpl.h"

namespace mozilla::ipc {

LazyLogModule gHWInferenceLog("HWInference");
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

HWInferenceChild::HWInferenceChild() {
  nsDebugImpl::SetMultiprocessMode("HWInference");
}

void HWInferenceChild::Bind(Endpoint<PHWInferenceChild>&& aEndpoint) {
  DebugOnly<bool> ok = aEndpoint.Bind(this);
  MOZ_ASSERT(ok, "HWInferenceChild::Bind: error");
}

void HWInferenceChild::Shutdown() { PHWInferenceChild::Close(); }

IPCResult HWInferenceChild::RecvNewContentHWInferenceManager(
    Endpoint<PHWInferenceManagerParent>&& aEndpoint,
    const dom::ContentParentId& aContentId) {
  LOGD("[{} - {}] Received connection request from content {}", fmt::ptr(this),
       __func__, static_cast<uint64_t>(aContentId));

  if (!HWInferenceManagerParent::CreateForContent(std::move(aEndpoint),
                                                  aContentId)) {
    LOGE(
        "[{} - {}]"
        "Error: Failed to create HWInferenceManagerParent, content id: {}",
        fmt::ptr(this), __func__, static_cast<uint64_t>(aContentId));
    return IPC_FAIL_NO_REASON(this);
  }

  LOGD("[{} - {}] Successfully created HWInferenceManagerParent for content {}",
       fmt::ptr(this), __func__, static_cast<uint64_t>(aContentId));
  return IPC_OK();
}

RefPtr<HWInferenceChild::IsModelAvailablePromise>
HWInferenceChild::SendIsModelAvailable(const nsCString& aModel,
                                       const nsCString& aRevision,
                                       const nsCString& aFilename) {
  LOGD(
      "[{} - {}] Sending model availability request to parent process: "
      "model={} revision={} filename={}",
      fmt::ptr(this), __func__, aModel.get(), aRevision.get(), aFilename.get());

  return PHWInferenceChild::SendIsModelAvailable(aModel, aRevision, aFilename);
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
HWInferenceChild::SendGetModelFile(const nsCString& aTask,
                                   const nsCString& aModel,
                                   const nsCString& aRevision,
                                   const nsCString& aFilename) {
  LOGD(
      "[{} - {}] Sending model file request to parent process: task={} "
      "model={} "
      "revision={} filename={}",
      fmt::ptr(this), __func__, aTask.get(), aModel.get(), aRevision.get(),
      aFilename.get());

  return PHWInferenceChild::SendGetModelFile(aTask, aModel, aRevision,
                                             aFilename);
}

}  // namespace mozilla::ipc

#undef LOG
