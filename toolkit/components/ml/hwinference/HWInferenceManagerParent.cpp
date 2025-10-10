/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceManagerParent.h"
#include "mozilla/Logging.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/SpeechRecognitionParent.h"

namespace mozilla::ipc {

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

  // The endpoint binds the actor, not the other way around
  if (!aEndpoint.Bind(parent)) {
    LOGE(
        "HWInferenceManagerParent::CreateForContent - Failed to bind endpoint");
    return false;
  }

  return true;
}

void HWInferenceManagerParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("[{}] HWInferenceManagerParent::ActorDestroy reason={}", (void*)this,
       static_cast<int>(aReason));
}

PSpeechRecognitionParent*
HWInferenceManagerParent::AllocPSpeechRecognitionParent() {
  LOGD("[{}] HWInferenceManagerParent::AllocPSpeechRecognitionParent",
       (void*)this);

  RefPtr<SpeechRecognitionParent> actor = new SpeechRecognitionParent();

  mSpeechSessions.AppendElement(actor);
  LOGD("[{}] Created SpeechRecognitionParent actor={:p}, total sessions={}",
       (void*)this, (void*)actor.get(), mSpeechSessions.Length());

  return actor.get();
}

bool HWInferenceManagerParent::DeallocPSpeechRecognitionParent(
    PSpeechRecognitionParent* aActor) {
  RefPtr<SpeechRecognitionParent> actor =
      static_cast<SpeechRecognitionParent*>(aActor);

  LOGD(
      "[{}] HWInferenceManagerParent::DeallocPSpeechRecognitionParent "
      "actor={:p}",
      (void*)this, (void*)aActor);

  mSpeechSessions.RemoveElement(actor);
  LOGD("[{}] Removed actor from array, remaining sessions={}", (void*)this,
       mSpeechSessions.Length());

  return true;
}

}  // namespace mozilla::ipc

#undef LOGD
#undef LOGE
