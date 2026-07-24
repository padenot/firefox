/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEMODELRESOLVER_H_
#define TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEMODELRESOLVER_H_

#include "nsIMLModelResolver.h"

namespace mozilla::hwinference {

// Example consumer's implementation of the generic HWInference model resolver,
// registered for the "browser-inference-example" task. Demonstrates how a
// consumer maps its own opaque model ids to concrete ModelHub artifacts so the
// (less trusted) utility process can only ever select among already-approved
// artifacts. The mapping here is a single made-up entry; a real consumer would
// resolve against an in-tree table.
class BrowserInferenceExampleModelResolver final : public nsIMLModelResolver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIMLMODELRESOLVER

  BrowserInferenceExampleModelResolver() = default;

 private:
  ~BrowserInferenceExampleModelResolver() = default;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEMODELRESOLVER_H_
