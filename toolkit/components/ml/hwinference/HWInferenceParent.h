/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_HWINFERENCE_HWINFERENCEPARENT_H_
#define TOOLKIT_COMPONENTS_ML_HWINFERENCE_HWINFERENCEPARENT_H_

#include "mozilla/ProcInfo.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/hwinference/PHWInferenceParent.h"
#include "mozilla/ipc/UtilityMediaService.h"

namespace mozilla::hwinference {

// HWInference parent process side
class HWInferenceParent final : public PHWInferenceParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceParent, override);

  explicit HWInferenceParent() = default;

  void ActorDestroy(ActorDestroyReason aReason) override;

  // This implements the `available` method of the SpeechRecognition object.
  mozilla::ipc::IPCResult RecvIsModelAvailable(
      nsCString&& aEngine, nsCString&& aModel, nsCString&& aRevision,
      nsCString&& aFilename, IsModelAvailableResolver&& aResolver);

  // Install (download) a model for a specific task
  mozilla::ipc::IPCResult RecvInstallModel(nsCString&& aTask,
                                           nsCString&& aModel,
                                           nsCString&& aRevision,
                                           nsCString&& aFilename,
                                           InstallModelResolver&& aResolver);
  // Get a model file as a file descriptor for use by inference engines
  mozilla::ipc::IPCResult RecvGetModelFile(nsCString&& aEngineId,
                                           nsCString&& aTask,
                                           nsCString&& aModel,
                                           nsCString&& aRevision,
                                           nsCString&& aFilename,
                                           GetModelFileResolver&& aResolver);

  ipc::UtilityActorName GetActorName() {
    return ipc::UtilityActorName::HwInference;
  }

  nsresult BindToUtilityProcess(
      const RefPtr<ipc::UtilityProcessParent>& aUtilityParent);

  static RefPtr<HWInferenceParent> GetSingleton();

 private:
  friend PHWInferenceParent;
  static StaticRefPtr<HWInferenceParent> sSingleton;
  ~HWInferenceParent() = default;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_HWINFERENCE_HWINFERENCEPARENT_H_
