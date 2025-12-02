/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HWInferenceManagerChild.h"
#include "mozilla/Logging.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ipc/SpeechRecognitionChild.h"
#include "mozilla/ipc/PSpeechRecognitionChild.h"

extern mozilla::LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

namespace mozilla::hwinference {

StaticRefPtr<HWInferenceManagerChild> HWInferenceManagerChild::sSingleton;

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

ipc::PSpeechRecognitionChild*
HWInferenceManagerChild::AllocPSpeechRecognitionChild() {
  RefPtr<SpeechRecognitionChild> actor = new SpeechRecognitionChild();

  mSpeechSessions.AppendElement(actor);
  LOGD("Created SpeechRecognitionChild actor={:p}, total={}",
       fmt::ptr(actor.get()), mSpeechSessions.Length());

  return actor.get();
}

bool HWInferenceManagerChild::DeallocPSpeechRecognitionChild(
    PSpeechRecognitionChild* aActor) {
  RefPtr<SpeechRecognitionChild> actor =
      static_cast<SpeechRecognitionChild*>(aActor);

  LOGD("Dealloc SpeechRecognitionChild actor={:p}", fmt::ptr(aActor));

  mSpeechSessions.RemoveElement(actor);
  return true;
}

RefPtr<SpeechRecognitionChild>
HWInferenceManagerChild::CreateSpeechRecognitionSession() {
  LOGD("{}", __func__);

  if (!CanSend()) {
    LOGE("{} - Cannot send", __func__);
    return nullptr;
  }

  RefPtr<SpeechRecognitionChild> actor = static_cast<SpeechRecognitionChild*>(
      SendPSpeechRecognitionConstructor(AllocPSpeechRecognitionChild()));

  if (actor) {
    LOGD("Successfully created SpeechRecognitionChild actor={:p}",
         fmt::ptr(actor.get()));
  } else {
    LOGE("Failed to create SpeechRecognitionChild");
  }

  return actor;
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGE
