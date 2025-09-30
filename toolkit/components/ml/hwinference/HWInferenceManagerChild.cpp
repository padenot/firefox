/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceManagerChild.h"
#include "mozilla/ipc/SpeechRecognitionChild.h"
#include "mozilla/Logging.h"
#include "mozilla/ipc/Endpoint.h"

namespace mozilla {
namespace ipc {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGV(fmt, ...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Verbose, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

MOZ_RUNINIT /* static */ RefPtr<HWInferenceManagerChild>
    HWInferenceManagerChild::sSingleton;

HWInferenceManagerChild::HWInferenceManagerChild() {
  LOGD("[{}] HWInferenceManagerChild::HWInferenceManagerChild", (void*)this);
}

/* static */
void HWInferenceManagerChild::OpenForProcess(
    Endpoint<PHWInferenceManagerChild>&& aEndpoint) {
  LOGD("HWInferenceManagerChild::OpenForProcess - [CONTENT] Opening connection to utility process");

  // Check if we already have a singleton that can send
  if (sSingleton && sSingleton->CanSend()) {
    LOGD(
        "HWInferenceManagerChild::OpenForProcess - [CONTENT] Already have active singleton, reusing");
    return;
  }

  // Clear any old singleton
  sSingleton = nullptr;

  // Create new manager child and bind endpoint
  if (aEndpoint.IsValid()) {
    LOGD("HWInferenceManagerChild::OpenForProcess - [CONTENT] Creating new manager and binding endpoint");
    RefPtr<HWInferenceManagerChild> manager = new HWInferenceManagerChild();
    if (aEndpoint.Bind(manager)) {
      sSingleton = manager;
      LOGD(
          "HWInferenceManagerChild::OpenForProcess - [CONTENT] Successfully bound endpoint, connection ready");
    } else {
      LOGE("HWInferenceManagerChild::OpenForProcess - [CONTENT] ERROR: Failed to bind endpoint");
    }
  } else {
    LOGE("HWInferenceManagerChild::OpenForProcess - [CONTENT] ERROR: Invalid endpoint received");
  }
}

/* static */
RefPtr<HWInferenceManagerChild> HWInferenceManagerChild::GetSingleton() {
  return sSingleton;
}

void HWInferenceManagerChild::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("[{}] HWInferenceManagerChild::ActorDestroy reason={}", (void*)this,
       static_cast<int>(aReason));

  // Clear singleton when destroyed
  if (sSingleton == this) {
    LOGD("Clearing singleton due to ActorDestroy");
    sSingleton = nullptr;
  }

  // Clear any active speech sessions
  mSpeechSessions.Clear();
}

PSpeechRecognitionChild*
HWInferenceManagerChild::AllocPSpeechRecognitionChild(
    const uint64_t& aSessionId) {
  LOGD("[{}] HWInferenceManagerChild::AllocPSpeechRecognitionChild session={}",
       (void*)this, aSessionId);

  RefPtr<SpeechRecognitionChild> actor =
      new SpeechRecognitionChild(aSessionId, this);

  // Store the actor in our session map
  mSpeechSessions.InsertOrUpdate(aSessionId, actor);
  LOGD("[{}] Created and stored SpeechRecognitionChild actor={:p} for session={}, total sessions={}",
       (void*)this, (void*)actor.get(), aSessionId, mSpeechSessions.Count());

  // Return raw pointer - IPDL will manage the reference
  return actor.get();
}

bool HWInferenceManagerChild::DeallocPSpeechRecognitionChild(
    PSpeechRecognitionChild* aActor) {
  RefPtr<SpeechRecognitionChild> actor =
      static_cast<SpeechRecognitionChild*>(aActor);
  uint64_t sessionId = actor->GetSessionId();

  LOGD("[{}] HWInferenceManagerChild::DeallocPSpeechRecognitionChild actor={:p} session={}",
       (void*)this, (void*)aActor, sessionId);

  // Remove from our session map
  bool removed = mSpeechSessions.Remove(sessionId);
  LOGD("[{}] Removed session={} from map: %s, remaining sessions={}",
       (void*)this, sessionId, removed ? "success" : "not found", mSpeechSessions.Count());

  return true;
}

RefPtr<SpeechRecognitionChild>
HWInferenceManagerChild::CreateSpeechRecognitionSession(
    uint64_t aSessionId) {
  LOGD("[{}] HWInferenceManagerChild::CreateSpeechRecognitionSession session={}",
       (void*)this, aSessionId);

  if (!CanSend()) {
    LOGE("[{}] HWInferenceManagerChild::CreateSpeechRecognitionSession - Cannot send for session={}",
         (void*)this, aSessionId);
    return nullptr;
  }

  // Send the creation message and get back the actor
  LOGD("[{}] Sending PSpeechRecognitionConstructor for session={}", (void*)this, aSessionId);
  RefPtr<SpeechRecognitionChild> actor =
      static_cast<SpeechRecognitionChild*>(SendPSpeechRecognitionConstructor(aSessionId));

  if (actor) {
    LOGD("[{}] Successfully created SpeechRecognitionChild actor={:p} for session={}",
         (void*)this, (void*)actor.get(), aSessionId);
  } else {
    LOGE("[{}] Failed to create SpeechRecognitionChild for session={}", (void*)this, aSessionId);
  }

  return actor;
}


}  // namespace ipc
}  // namespace mozilla
