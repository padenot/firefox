/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserInferenceExampleModelResolver.h"

namespace mozilla::hwinference {

NS_IMPL_ISUPPORTS(BrowserInferenceExampleModelResolver, nsIMLModelResolver)

NS_IMETHODIMP
BrowserInferenceExampleModelResolver::Resolve(const nsACString& aId,
                                              nsACString& aEngine,
                                              nsACString& aModel,
                                              nsACString& aRevision,
                                              nsACString& aFilename) {
  // A real consumer resolves against an in-tree table; the example knows a
  // single id.
  if (!aId.EqualsLiteral("demo-model")) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  aEngine.AssignLiteral("browser-inference-example");
  aModel.AssignLiteral("example-org/demo-model");
  aRevision.AssignLiteral("v1");
  aFilename.AssignLiteral("demo.gguf");
  return NS_OK;
}

}  // namespace mozilla::hwinference
