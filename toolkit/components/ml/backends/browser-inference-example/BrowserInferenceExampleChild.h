/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLECHILD_H_
#define TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLECHILD_H_

#include "mozilla/MozPromise.h"
#include "mozilla/hwinference/PBrowserInferenceExampleChild.h"

namespace mozilla::hwinference {

class BrowserInferenceExampleChild final
    : public PBrowserInferenceExampleChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(BrowserInferenceExampleChild, override);

  using SmokeTestPromise = MozPromise<float, nsresult, true>;

  RefPtr<SmokeTestPromise> RunScalar(float aInput);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  friend PBrowserInferenceExampleChild;
  ~BrowserInferenceExampleChild() = default;

  void Close();

  bool mActorDestroyed = false;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLECHILD_H_
