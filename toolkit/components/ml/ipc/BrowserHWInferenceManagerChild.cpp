/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserHWInferenceManagerChild.h"

#include "mozilla/Logging.h"
#include "mozilla/hwinference/BrowserInferenceExampleChild.h"
#include "nsThreadUtils.h"

namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

/* static */
RefPtr<BrowserHWInferenceManagerChild::SmokeTestPromise>
BrowserHWInferenceManagerChild::RunSmokeTest(
    ipc::Endpoint<PBrowserHWInferenceManagerChild>&& aEndpoint, float aInput) {
  RefPtr<BrowserHWInferenceManagerChild> manager =
      new BrowserHWInferenceManagerChild();
  if (!aEndpoint.Bind(manager)) {
    return SmokeTestPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }
  return manager->RunSmokeTest(aInput);
}

RefPtr<BrowserHWInferenceManagerChild::SmokeTestPromise>
BrowserHWInferenceManagerChild::RunSmokeTest(float aInput) {
  if (!CanSend()) {
    return SmokeTestPromise::CreateAndReject(NS_ERROR_NOT_AVAILABLE, __func__);
  }

  RefPtr<BrowserInferenceExampleChild> child =
      new BrowserInferenceExampleChild();
  RefPtr<BrowserInferenceExampleChild> actor =
      static_cast<BrowserInferenceExampleChild*>(
          SendPBrowserInferenceExampleConstructor(child.get()));
  if (!actor) {
    Close();
    return SmokeTestPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  return actor->RunScalar(aInput)->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [self = RefPtr{this}](float aResult) {
        self->Close();
        return SmokeTestPromise::CreateAndResolve(aResult, __func__);
      },
      [self = RefPtr{this}](nsresult aRv) {
        self->Close();
        return SmokeTestPromise::CreateAndReject(aRv, __func__);
      });
}

void BrowserHWInferenceManagerChild::Close() {
  if (mActorDestroyed || !CanSend()) {
    return;
  }
  PBrowserHWInferenceManagerChild::Close();
}

void BrowserHWInferenceManagerChild::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("BrowserHWInferenceManagerChild::ActorDestroy reason={}",
       static_cast<int>(aReason));
  mActorDestroyed = true;
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGE
