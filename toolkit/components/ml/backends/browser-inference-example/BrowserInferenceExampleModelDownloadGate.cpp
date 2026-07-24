/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserInferenceExampleModelDownloadGate.h"

#include "mozilla/Logging.h"
#include "nsString.h"

namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)

NS_IMPL_ISUPPORTS(BrowserInferenceExampleModelDownloadGate,
                  nsIMLModelDownloadGate)

NS_IMETHODIMP BrowserInferenceExampleModelDownloadGate::ShouldAllowDownload(
    const nsACString& aTask, const nsACString& aModel,
    const nsACString& aRevision, const nsACString& aFilename,
    uint64_t aInnerWindowId, uint64_t aContentId, const nsAString& aProgressToken,
    nsIMLModelDownloadGateCallback* aCallback) {
  LOGD(
      "BrowserInferenceExampleModelDownloadGate - allowing download task={} "
      "model={} revision={} filename={} innerWindowId={} contentId={}",
      PromiseFlatCString(aTask).get(), PromiseFlatCString(aModel).get(),
      PromiseFlatCString(aRevision).get(), PromiseFlatCString(aFilename).get(),
      aInnerWindowId, aContentId);
  // This example is driven from chrome (a privileged parent-process caller),
  // not from a content document, so aContentId and aInnerWindowId are 0: there
  // is no requesting content to validate, and the origin is already trusted. A
  // content-facing consumer would instead validate that aInnerWindowId belongs
  // to aContentId and show consent UI, answering asynchronously. The example
  // authorizes immediately.
  aCallback->Resolve(true);
  return NS_OK;
}

}  // namespace mozilla::hwinference

#undef LOGD
