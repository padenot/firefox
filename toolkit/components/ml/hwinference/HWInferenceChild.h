/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __include_ipc_glue_HWInferenceChild_h_
#define __include_ipc_glue_HWInferenceChild_h_

#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/PHWInferenceChild.h"
#include "mozilla/ipc/UtilityProcessSandboxing.h"
#include "mozilla/ipc/UtilityMediaService.h"
#include "mozilla/dom/ipc/IdType.h"

namespace mozilla::ipc {

/**
 * HWInferenceChild manages hardware inference services in the utility process.
 *
 * Architecture overview:
 * - HWInferenceChild runs in a dedicated utility process for hardware inference
 * - Content processes communicate with HWInferenceManagerChild
 * - HWInferenceManagerChild forwards requests through HWInferenceManagerParent
 * - HWInferenceManagerParent runs in the main process and routes requests to
 *   HWInferenceParent, which then forwards them to HWInferenceChild
 * - This architecture isolates ML/inference workloads in a separate process
 *   with appropriate sandbox settings while allowing multiple content processes
 *   to share the same inference service
 *
 * Communication flow:
 * Content Process -> HWInferenceManagerChild -> HWInferenceManagerParent (main)
 *   -> HWInferenceParent (main) -> HWInferenceChild (utility process)
 */
class HWInferenceChild final : public PHWInferenceChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceChild, override);

  HWInferenceChild();

  void Bind(Endpoint<PHWInferenceChild>&& aEndpoint);
  void Shutdown();

  mozilla::ipc::IPCResult RecvNewContentHWInferenceManager(
      Endpoint<PHWInferenceManagerParent>&& aEndpoint,
      const dom::ContentParentId& aContentId);

  UtilityActorName GetActorName() { return UtilityActorName::HwInference; }

 private:
  friend PHWInferenceChild;
  ~HWInferenceChild() = default;
};

}  // namespace mozilla::ipc

#endif  // __include_ipc_glue_HWInferenceChild_h_
