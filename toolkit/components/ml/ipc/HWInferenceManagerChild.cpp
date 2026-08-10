/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/hwinference/HWInferenceManagerChild.h"
#include "mozilla/Logging.h"
#include "mozilla/hwinference/PHWInferenceManager.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/StaticPtr.h"
#include "nsThreadUtils.h"
#include "mozilla/hwinference/SpeechRecognitionChild.h"

namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

StaticRefPtr<HWInferenceManagerChild> HWInferenceManagerChild::sSingleton;
uint32_t HWInferenceManagerChild::sConnectionUsers = 0;

/* static */
already_AddRefed<HWInferenceConnectionGuard>
HWInferenceManagerChild::AcquireConnection() {
  AssertIsOnMainThread();
  return RefPtr<HWInferenceConnectionGuard>(new HWInferenceConnectionGuard())
      .forget();
}

/* static */
void HWInferenceManagerChild::AcquireConnectionUser() {
  AssertIsOnMainThread();
  sConnectionUsers++;
  EnsureConnected();
}

/* static */
void HWInferenceManagerChild::ReleaseConnectionUser() {
  AssertIsOnMainThread();
  MOZ_ASSERT(sConnectionUsers > 0);
  if (--sConnectionUsers) {
    return;
  }
  // Nothing needs the utility process anymore: closing gives up this process'
  // reference on it, from ActorDestroy.
  if (sSingleton) {
    LOGD("{} - Last connection user gone, closing", __func__);
    sSingleton->Close();
  }
}

/* static */
void HWInferenceManagerChild::EnsureConnected() {
  AssertIsOnMainThread();

  if (sSingleton && sSingleton->CanSend()) {
    return;
  }

  Endpoint<PHWInferenceManagerParent> parentEp;
  Endpoint<PHWInferenceManagerChild> childEp;
  MOZ_ALWAYS_SUCCEEDS(
      PHWInferenceManager::CreateEndpoints(&parentEp, &childEp));
  if (!OpenForProcess(std::move(childEp))) {
    return;
  }

  // Null in the parent process, which never connects this way.
  if (dom::ContentChild* contentChild = dom::ContentChild::GetSingleton()) {
    contentChild->SendRequestHWInferenceConnection(std::move(parentEp));
  }
}

HWInferenceConnectionGuard::HWInferenceConnectionGuard() {
  HWInferenceManagerChild::AcquireConnectionUser();
}

HWInferenceConnectionGuard::~HWInferenceConnectionGuard() {
  if (NS_IsMainThread()) {
    HWInferenceManagerChild::ReleaseConnectionUser();
  } else {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "HWInferenceConnectionGuard::Release",
        [] { HWInferenceManagerChild::ReleaseConnectionUser(); }));
  }
}

/* static */
void HWInferenceManagerChild::ReleaseConnectionReference() {
  AssertIsOnMainThread();
  // Null in the parent process, which holds its reference directly.
  if (dom::ContentChild* contentChild = dom::ContentChild::GetSingleton()) {
    (void)contentChild->SendReleaseHWInferenceConnection();
  }
}

/* static */
bool HWInferenceManagerChild::OpenForProcess(
    Endpoint<PHWInferenceManagerChild>&& aEndpoint) {
  AssertIsOnMainThread();
  LOGD("{} - Opening connection to utility process", __func__);

  if (sSingleton && sSingleton->CanSend()) {
    LOGD("{} - Already have active singleton, reusing", __func__);
    return false;
  }

  sSingleton = nullptr;

  if (aEndpoint.IsValid()) {
    LOGD("Creating new manager and binding endpoint");
    RefPtr<HWInferenceManagerChild> manager = new HWInferenceManagerChild();
    if (aEndpoint.Bind(manager)) {
      sSingleton = manager;
      LOGD("Successfully bound endpoint, connection ready", __func__);
      return true;
    }
    LOGE("{} - ERROR: Failed to bind endpoint", __func__);
  } else {
    LOGE("{} - ERROR: Invalid endpoint received", __func__);
  }

  return false;
}

/* static */
RefPtr<HWInferenceManagerChild> HWInferenceManagerChild::GetSingleton() {
  AssertIsOnMainThread();
  return sSingleton;
}

void HWInferenceManagerChild::ActorDestroy(ActorDestroyReason aReason) {
  AssertIsOnMainThread();
  LOGD("{} reason={}, clearing singleton", __func__, static_cast<int>(aReason));

  // OpenForProcess() replaces a singleton that can no longer send, so an
  // ActorDestroy arriving after that must not clear its replacement.
  if (sSingleton == this) {
    sSingleton = nullptr;
  }

  ReleaseConnectionReference();
}

RefPtr<SpeechRecognitionSessionPromise>
HWInferenceManagerChild::CreateSpeechRecognitionSession(
    nsISerialEventTarget* aTarget) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aTarget);
  LOGD("{}", __func__);

  if (!CanSend()) {
    LOGE("{} - Cannot send", __func__);
    return SpeechRecognitionSessionPromise::CreateAndReject(
        NS_ERROR_NOT_AVAILABLE, __func__);
  }

  RefPtr<SpeechRecognitionSessionPromise::Private> session =
      new SpeechRecognitionSessionPromise::Private(__func__);

  SendCreateSpeechRecognition()->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [session, target = nsCOMPtr<nsISerialEventTarget>(aTarget)](
          CreateSpeechRecognitionPromise::ResolveOrRejectValue&&
              aValue) mutable {
        if (aValue.IsReject() || !aValue.ResolveValue().IsValid()) {
          LOGE("Failed to obtain a PSpeechRecognition endpoint");
          session->Reject(NS_ERROR_FAILURE, __func__);
          return;
        }

        nsCOMPtr<nsIRunnable> bind = NS_NewRunnableFunction(
            "HWInferenceManagerChild::BindSpeechRecognitionSession",
            [session, endpoint = std::move(aValue.ResolveValue())]() mutable {
              RefPtr<SpeechRecognitionChild> actor =
                  new SpeechRecognitionChild();
              if (!endpoint.Bind(actor)) {
                LOGE("Failed to bind SpeechRecognitionChild");
                session->Reject(NS_ERROR_FAILURE, __func__);
                return;
              }
              LOGD("Successfully created SpeechRecognitionChild actor={:p}",
                   fmt::ptr(actor.get()));
              session->Resolve(std::move(actor), __func__);
            });
        if (NS_FAILED(target->Dispatch(bind.forget()))) {
          session->Reject(NS_ERROR_FAILURE, __func__);
        }
      });

  return session;
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGE
