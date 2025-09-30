/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceChild.h"
#include "HWInferenceManagerParent.h"
#include "mozilla/MozPromise.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/Logging.h"
#include "nsDebugImpl.h"

namespace mozilla::ipc {

LazyLogModule gHWInferenceLog("HWInference");
#define HWINF_LOG(...) MOZ_LOG(gHWInferenceLog, LogLevel::Debug, (__VA_ARGS__))

HWInferenceChild::~HWInferenceChild() = default;

HWInferenceChild::HWInferenceChild() {
  nsDebugImpl::SetMultiprocessMode("HWInference");
  HWINF_LOG(
      "[%p] HWInferenceChild: Constructor called - utility process starting",
      this);
}

void HWInferenceChild::Bind(Endpoint<PHWInferenceChild>&& aEndpoint) {
  HWINF_LOG("[%p] HWInferenceChild: Bind called - IPC connection established",
            this);
  DebugOnly<bool> ok = aEndpoint.Bind(this);
  MOZ_ASSERT(ok);
}

void HWInferenceChild::Shutdown() { PHWInferenceChild::Close(); }

mozilla::ipc::IPCResult HWInferenceChild::RecvNewContentHWInferenceManager(
    Endpoint<PHWInferenceManagerParent>&& aEndpoint,
    const dom::ContentParentId& aContentId) {
  HWINF_LOG(
      "[%p] HWInferenceChild::RecvNewContentHWInferenceManager - [UTILITY] STEP 2: Received connection request from content %d",
      this, static_cast<int>(aContentId));

  if (!HWInferenceManagerParent::CreateForContent(std::move(aEndpoint),
                                                  aContentId)) {
    HWINF_LOG("[%p] HWInferenceChild::RecvNewContentHWInferenceManager - [UTILITY] ERROR: Failed to create HWInferenceManagerParent", this);
    return IPC_FAIL_NO_REASON(this);
  }

  HWINF_LOG("[%p] HWInferenceChild::RecvNewContentHWInferenceManager - [UTILITY] STEP 2.1: Successfully created HWInferenceManagerParent for content", this);
  return IPC_OK();
}

RefPtr<HWInferenceChild::IsModelAvailablePromise>
HWInferenceChild::SendIsModelAvailable(const nsCString& aModel, const nsCString& aRevision, const nsCString& aFilename) {
  HWINF_LOG("[%p] HWInferenceChild::SendIsModelAvailable - [UTILITY] Sending model availability request to parent process: model=%s revision=%s",
            this, aModel.get(), aRevision.get());

  return PHWInferenceChild::SendIsModelAvailable(aModel, aRevision, aFilename);
}

RefPtr<HWInferenceChild::InstallModelPromise>
HWInferenceChild::SendInstallModel(const nsCString& aModel, const nsCString& aRevision, const nsCString& aFilename) {
  HWINF_LOG("[%p] HWInferenceChild::SendInstallModel - [UTILITY] Sending model installation request to parent process: model=%s revision=%s filename=%s",
            this, aModel.get(), aRevision.get(), aFilename.get());

  return PHWInferenceChild::SendInstallModel(aModel, aRevision, aFilename);
}

RefPtr<HWInferenceChild::GetModelBlobPromise>
HWInferenceChild::SendGetModelBlob(const nsCString& aModel, const nsCString& aRevision, const nsCString& aFilename) {
  HWINF_LOG("[%p] HWInferenceChild::SendGetModelBlob - [UTILITY] Sending model blob request to parent process: model=%s revision=%s filename=%s",
            this, aModel.get(), aRevision.get(), aFilename.get());

  return PHWInferenceChild::SendGetModelBlob(aModel, aRevision, aFilename);
}

}  // namespace mozilla::ipc
