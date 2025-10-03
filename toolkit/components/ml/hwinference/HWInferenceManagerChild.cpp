/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceManagerChild.h"
#include "mozilla/ipc/SpeechRecognitionChild.h"
#include "mozilla/Logging.h"
#include "mozilla/ipc/Endpoint.h"

namespace mozilla::ipc {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

MOZ_RUNINIT /* static */ RefPtr<HWInferenceManagerChild>
    HWInferenceManagerChild::sSingleton;

/* static */
void HWInferenceManagerChild::OpenForProcess(
    Endpoint<PHWInferenceManagerChild>&& aEndpoint) {
  LOGD("{} - Opening connection to utility process", __func__);

  if (sSingleton && sSingleton->CanSend()) {
    LOGD("{} - Already have active singleton, reusing", __func__);
    return;
  }

  sSingleton = nullptr;

  if (aEndpoint.IsValid()) {
    LOGD("Creating new manager and binding endpoint");
    RefPtr<HWInferenceManagerChild> manager = new HWInferenceManagerChild();
    if (aEndpoint.Bind(manager)) {
      sSingleton = manager;
      LOGD("Successfully bound endpoint, connection ready", __func__);
    } else {
      LOGE("{} - ERROR: Failed to bind endpoint", __func__);
    }
  } else {
    LOGE("{} - ERROR: Invalid endpoint received", __func__);
  }
}

/* static */
RefPtr<HWInferenceManagerChild> HWInferenceManagerChild::GetSingleton() {
  return sSingleton;
}

void HWInferenceManagerChild::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("{} reason={}, clearing singleton", __func__, static_cast<int>(aReason));

  mSpeechSessions.Clear();
  sSingleton = nullptr;
}

PSpeechRecognitionChild* HWInferenceManagerChild::AllocPSpeechRecognitionChild(
    const uint64_t& aSessionId) {
  RefPtr<SpeechRecognitionChild> actor =
      new SpeechRecognitionChild(aSessionId, this);

  mSpeechSessions.InsertOrUpdate(aSessionId, actor);
  LOGD(
      "Created and stored SpeechRecognitionChild actor={:p} for session={}, "
      "session count={}",
      fmt::ptr(actor.get()), aSessionId, mSpeechSessions.Count());

  return actor.get();
}

bool HWInferenceManagerChild::DeallocPSpeechRecognitionChild(
    PSpeechRecognitionChild* aActor) {
  RefPtr<SpeechRecognitionChild> actor =
      static_cast<SpeechRecognitionChild*>(aActor);
  uint64_t sessionId = actor->GetSessionId();

  bool removed = mSpeechSessions.Remove(sessionId);
  LOGD(
      "Dealloc SpeechRecognitionChild actor={:p} for session={}, session "
      "count={}: {}",
      fmt::ptr(actor.get()), sessionId, mSpeechSessions.Count(),
      removed ? "success" : "not found");

  return true;
}

RefPtr<SpeechRecognitionChild>
HWInferenceManagerChild::CreateSpeechRecognitionSession(uint64_t aSessionId) {
  LOGD("{} session={}", __func__, aSessionId);

  if (!CanSend()) {
    LOGE("{} - Cannot send for session={}", __func__, aSessionId);
    return nullptr;
  }

  RefPtr<SpeechRecognitionChild> actor = static_cast<SpeechRecognitionChild*>(
      SendPSpeechRecognitionConstructor(aSessionId));

  if (actor) {
    LOGD(
        "Successfully created SpeechRecognitionChild actor={:p} for session={}",
        fmt::ptr(actor.get()), aSessionId);
  } else {
    LOGE("Failed to create SpeechRecognitionChild for session={}", aSessionId);
  }

  return actor;
}

}  // namespace mozilla::ipc

#undef LOGD
#undef LOGE
