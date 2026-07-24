/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserInferenceExampleChild.h"

#include "mozilla/Logging.h"
#include "nsThreadUtils.h"

namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

RefPtr<BrowserInferenceExampleChild::SmokeTestPromise>
BrowserInferenceExampleChild::RunScalar(float aInput) {
  if (!CanSend()) {
    return SmokeTestPromise::CreateAndReject(NS_ERROR_NOT_AVAILABLE, __func__);
  }

  return SendRunScalar(aInput)->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [self = RefPtr{this}](BrowserInferenceExampleResult&& aResult) {
        self->Close();
        if (!aResult.ok()) {
          LOGE("Browser inference example failed: {}", aResult.error());
          return SmokeTestPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
        }
        return SmokeTestPromise::CreateAndResolve(aResult.value(), __func__);
      },
      [self = RefPtr{this}](ResponseRejectReason aReason) {
        self->Close();
        LOGE("Browser inference example IPC failed: {}",
             static_cast<int>(aReason));
        return SmokeTestPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
      });
}

void BrowserInferenceExampleChild::Close() {
  if (mActorDestroyed || !CanSend()) {
    return;
  }
  (void)Send__delete__(this);
}

void BrowserInferenceExampleChild::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("BrowserInferenceExampleChild::ActorDestroy reason={}",
       static_cast<int>(aReason));
  mActorDestroyed = true;
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGE
