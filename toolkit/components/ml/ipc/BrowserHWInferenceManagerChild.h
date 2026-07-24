/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_IPC_BROWSERHWINFERENCEMANAGERCHILD_H_
#define TOOLKIT_COMPONENTS_ML_IPC_BROWSERHWINFERENCEMANAGERCHILD_H_

#include "mozilla/hwinference/BrowserInferenceExampleChild.h"
#include "mozilla/hwinference/PBrowserHWInferenceManagerChild.h"
#include "mozilla/ipc/Endpoint.h"

namespace mozilla::hwinference {

class BrowserHWInferenceManagerChild final
    : public PBrowserHWInferenceManagerChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(BrowserHWInferenceManagerChild,
                                        override);

  using SmokeTestPromise = BrowserInferenceExampleChild::SmokeTestPromise;

  static RefPtr<SmokeTestPromise> RunSmokeTest(
      ipc::Endpoint<PBrowserHWInferenceManagerChild>&& aEndpoint, float aInput);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  ~BrowserHWInferenceManagerChild() = default;

  RefPtr<SmokeTestPromise> RunSmokeTest(float aInput);
  void Close();

  bool mActorDestroyed = false;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_IPC_BROWSERHWINFERENCEMANAGERCHILD_H_
