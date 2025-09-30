/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __include_ipc_glue_HWInferenceParent_h_
#define __include_ipc_glue_HWInferenceParent_h_

#include "mozilla/ProcInfo.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/PHWInferenceParent.h"
#include "mozilla/ipc/UtilityMediaService.h"
#include "mozilla/dom/ipc/IdType.h"

namespace mozilla::ipc {

class HWInferenceParent final : public PHWInferenceParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceParent, override);

  explicit HWInferenceParent();

  void ActorDestroy(ActorDestroyReason aReason) override;

  void Bind(Endpoint<PHWInferenceParent>&& aEndpoint);

  // PHWInferenceParent implementation
  mozilla::ipc::IPCResult RecvIsModelAvailable(
      nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
      IsModelAvailableResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInstallModel(
      nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
      InstallModelResolver&& aResolver);
  mozilla::ipc::IPCResult RecvGetModelBlob(
      nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
      GetModelBlobResolver&& aResolver);

  UtilityActorName GetActorName() { return UtilityActorName::HwInference; }

  nsresult BindToUtilityProcess(
      const RefPtr<UtilityProcessParent>& aUtilityParent);

  static RefPtr<HWInferenceParent> GetSingleton();

 private:
  friend PHWInferenceParent;
  ~HWInferenceParent();
};

}  // namespace mozilla::ipc

#endif  // __include_ipc_glue_HWInferenceParent_h_
