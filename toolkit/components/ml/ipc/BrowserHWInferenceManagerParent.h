/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_IPC_BROWSERHWINFERENCEMANAGERPARENT_H_
#define TOOLKIT_COMPONENTS_ML_IPC_BROWSERHWINFERENCEMANAGERPARENT_H_

#include "mozilla/hwinference/PBrowserHWInferenceManagerParent.h"
#include "mozilla/ipc/Endpoint.h"

namespace mozilla::hwinference {

class BrowserHWInferenceManagerParent final
    : public PBrowserHWInferenceManagerParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(BrowserHWInferenceManagerParent,
                                        override);

  static bool CreateForBrowser(
      ipc::Endpoint<PBrowserHWInferenceManagerParent>&& aEndpoint);

  already_AddRefed<PBrowserInferenceExampleParent>
  AllocPBrowserInferenceExampleParent();

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  ~BrowserHWInferenceManagerParent() = default;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_IPC_BROWSERHWINFERENCEMANAGERPARENT_H_
