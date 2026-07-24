/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserHWInferenceManagerParent.h"

#include "mozilla/Logging.h"
#include "mozilla/hwinference/BrowserInferenceExampleParent.h"

namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

/* static */
bool BrowserHWInferenceManagerParent::CreateForBrowser(
    ipc::Endpoint<PBrowserHWInferenceManagerParent>&& aEndpoint) {
  RefPtr<BrowserHWInferenceManagerParent> parent =
      new BrowserHWInferenceManagerParent();
  if (!aEndpoint.Bind(parent)) {
    LOGE("BrowserHWInferenceManagerParent::CreateForBrowser failed to bind");
    return false;
  }
  return true;
}

already_AddRefed<PBrowserInferenceExampleParent>
BrowserHWInferenceManagerParent::AllocPBrowserInferenceExampleParent() {
  RefPtr<BrowserInferenceExampleParent> actor =
      new BrowserInferenceExampleParent();
  return actor.forget();
}

void BrowserHWInferenceManagerParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("BrowserHWInferenceManagerParent::ActorDestroy reason={}",
       static_cast<int>(aReason));
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGE
