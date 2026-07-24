/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEMODELDOWNLOADGATE_H_
#define TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEMODELDOWNLOADGATE_H_

#include "nsIMLModelDownloadGate.h"

namespace mozilla::hwinference {

// Example consumer's implementation of the generic HWInference download gate,
// registered for the "browser-inference-example" task. Demonstrates the
// authorization hook the infrastructure consults before downloading a model on
// behalf of a requesting process. A real consumer would show consent UI and
// answer asynchronously; this example authorizes unconditionally (synchronously)
// and just logs the request.
class BrowserInferenceExampleModelDownloadGate final
    : public nsIMLModelDownloadGate {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIMLMODELDOWNLOADGATE

  BrowserInferenceExampleModelDownloadGate() = default;

 private:
  ~BrowserInferenceExampleModelDownloadGate() = default;
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_BACKENDS_BROWSER_INFERENCE_EXAMPLE_BROWSERINFERENCEEXAMPLEMODELDOWNLOADGATE_H_
