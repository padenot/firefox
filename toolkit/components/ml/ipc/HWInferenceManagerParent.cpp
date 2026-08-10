/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceManagerParent.h"
#include "mozilla/Logging.h"
#include "mozilla/hwinference/PSpeechRecognition.h"
#include "mozilla/hwinference/SpeechRecognitionParent.h"
#include "mozilla/ipc/Endpoint.h"
#include "nsDebug.h"

namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

HWInferenceManagerParent::HWInferenceManagerParent(
    dom::ContentParentId aContentId)
    : mContentId(aContentId) {
  LOGD("{} for content {}", __func__, static_cast<int>(mContentId));
}

/* static */
bool HWInferenceManagerParent::CreateForContent(
    Endpoint<PHWInferenceManagerParent>&& aEndpoint,
    dom::ContentParentId aContentId) {
  LOGD("HWInferenceManagerParent::CreateForContent for content {}",
       static_cast<int>(aContentId));

  RefPtr<HWInferenceManagerParent> parent =
      new HWInferenceManagerParent(aContentId);

  if (!aEndpoint.Bind(parent)) {
    LOGE(
        "HWInferenceManagerParent::CreateForContent - Failed to bind endpoint");
    return false;
  }

  return true;
}

ipc::IPCResult HWInferenceManagerParent::RecvCreateSpeechRecognition(
    CreateSpeechRecognitionResolver&& aResolver) {
  LOGD("[{}] HWInferenceManagerParent::RecvCreateSpeechRecognition",
       (void*)this);

  Endpoint<PSpeechRecognitionParent> parentEndpoint;
  Endpoint<PSpeechRecognitionChild> childEndpoint;
  if (NS_WARN_IF(NS_FAILED(PSpeechRecognition::CreateEndpoints(
          &parentEndpoint, &childEndpoint)))) {
    LOGE("[{}] Failed to create PSpeechRecognition endpoints", (void*)this);
    aResolver(Endpoint<PSpeechRecognitionChild>());
    return IPC_OK();
  }

  // mContentId is the id the parent process assigned to this connection, never
  // anything content sent. The new session inherits it, so it stays subject to
  // the same permission checks a managed actor used to get implicitly through
  // its manager.
  RefPtr<SpeechRecognitionParent> actor =
      new SpeechRecognitionParent(mContentId);
  if (!parentEndpoint.Bind(actor)) {
    LOGE("[{}] Failed to bind SpeechRecognitionParent", (void*)this);
    aResolver(Endpoint<PSpeechRecognitionChild>());
    return IPC_OK();
  }

  LOGD("[{}] Created SpeechRecognitionParent actor={:p}", (void*)this,
       (void*)actor.get());
  aResolver(std::move(childEndpoint));
  return IPC_OK();
}

void HWInferenceManagerParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("[{}] HWInferenceManagerParent::ActorDestroy reason={}", (void*)this,
       static_cast<int>(aReason));
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGE
