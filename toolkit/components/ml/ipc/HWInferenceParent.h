/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_IPC_HWINFERENCEPARENT_H_
#define TOOLKIT_COMPONENTS_ML_IPC_HWINFERENCEPARENT_H_

#include "mozilla/ProcInfo.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/hwinference/PHWInferenceParent.h"
#include "mozilla/ipc/UtilityMediaService.h"
#include "nsTHashMap.h"

namespace mozilla::hwinference {

// The HWInference process instances. "browser" serves inference driven by
// chrome, "content" serves inference driven by web content; they are separate
// processes so a compromised content-driven one cannot reach what the
// chrome-driven one holds.
#define HWINFERENCE_BROWSER_INSTANCE_KEY "browser"_ns
#define HWINFERENCE_CONTENT_INSTANCE_KEY "content"_ns

// HWInference parent process side
class HWInferenceParent final : public PHWInferenceParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceParent, override);

  explicit HWInferenceParent(const nsACString& aInstanceKey)
      : mInstanceKey(aInstanceKey) {}

  void ActorDestroy(ActorDestroyReason aReason) override;

  mozilla::ipc::IPCResult RecvIsModelAvailable(
      nsCString&& aTask, nsCString&& aId, IsModelAvailableResolver&& aResolver);

  mozilla::ipc::IPCResult RecvIsModelInstalled(
      nsCString&& aTask, nsCString&& aId, IsModelInstalledResolver&& aResolver);

  mozilla::ipc::IPCResult RecvInstallModel(
      nsCString&& aTask, nsCString&& aId, uint64_t aInnerWindowId,
      const dom::ContentParentId& aContentId, nsString&& aProgressToken,
      InstallModelResolver&& aResolver);

  mozilla::ipc::IPCResult RecvGetModelFile(nsCString&& aTask, nsCString&& aId,
                                           GetModelFileResolver&& aResolver);

  ipc::UtilityActorName GetActorName() {
    return ipc::UtilityActorName::HwInference;
  }

  nsresult BindToUtilityProcess(
      const RefPtr<ipc::UtilityProcessParent>& aUtilityParent);

  static RefPtr<HWInferenceParent> GetSingleton(const nsACString& aInstanceKey);

 private:
  friend PHWInferenceParent;
  static StaticAutoPtr<nsTHashMap<nsCStringHashKey, RefPtr<HWInferenceParent>>>
      sInstances;
  nsCString mInstanceKey;
  ~HWInferenceParent() = default;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_IPC_HWINFERENCEPARENT_H_
