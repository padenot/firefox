/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __include_ipc_glue_HWInferenceParent_h_
#define __include_ipc_glue_HWInferenceParent_h_

#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/ipc/PHWInferenceParent.h"
#include "mozilla/ipc/UtilityMediaService.h"

namespace mozilla::ipc {

// HWInference parent process side
class HWInferenceParent final : public PHWInferenceParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceParent, override);

  explicit HWInferenceParent() = default;

  void ActorDestroy(ActorDestroyReason aReason) override;

  // This implements the `available` method of the SpeechRecognition object.
  mozilla::ipc::IPCResult RecvIsModelAvailable(
      nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
      IsModelAvailableResolver&& aResolver);
  // This implements the `install` method of the SpeechRecognition object, and
  // initiates the (large) download of a model for a specific task.
  mozilla::ipc::IPCResult RecvInstallModel(nsCString&& aTask,
                                           nsCString&& aModel,
                                           nsCString&& aRevision,
                                           nsCString&& aFilename,
                                           InstallModelResolver&& aResolver);

  UtilityActorName GetActorName() { return UtilityActorName::HwInference; }

  nsresult BindToUtilityProcess(
      const RefPtr<UtilityProcessParent>& aUtilityParent);

  static RefPtr<HWInferenceParent> GetSingleton();

 private:
  friend PHWInferenceParent;
  static StaticRefPtr<HWInferenceParent> sSingleton;
  ~HWInferenceParent() = default;
};

}  // namespace mozilla::ipc

#endif  // __include_ipc_glue_HWInferenceParent_h_
