/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEPARENT_H_
#define TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEPARENT_H_

#include "mozilla/hwinference/PBrowserInferenceExampleParent.h"

namespace mozilla::hwinference {

class BrowserInferenceExampleParent final
    : public PBrowserInferenceExampleParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(BrowserInferenceExampleParent,
                                        override);

  mozilla::ipc::IPCResult RecvRunScalar(float aInput,
                                        RunScalarResolver&& aResolver);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  friend PBrowserInferenceExampleParent;
  ~BrowserInferenceExampleParent() = default;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEPARENT_H_
