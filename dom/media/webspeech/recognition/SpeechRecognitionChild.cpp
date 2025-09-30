/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionChild.h"

#include "mozilla/Logging.h"
#include "mozilla/MozPromise.h"
#include "mozilla/ipc/HWInferenceManagerChild.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "nsDebug.h"

namespace mozilla::ipc {

static LazyLogModule gSpeechRecognitionChildLog("SpeechRecognitionChild");
#define SRCHILD_LOG(level, fmt, ...)         \
  MOZ_LOG(gSpeechRecognitionChildLog, level, \
          ("[SRChild:%p:%lu] " fmt, this,    \
           static_cast<unsigned long>(mSessionId), ##__VA_ARGS__))

SpeechRecognitionChild::SpeechRecognitionChild(
    uint64_t aSessionId, HWInferenceManagerChild* aManager)
    : mSessionId(aSessionId), mManager(aManager) {
  SRCHILD_LOG(LogLevel::Debug, "Constructor called with manager=%p", aManager);
  MOZ_ASSERT(mManager);
}

SpeechRecognitionChild::~SpeechRecognitionChild() {
  SRCHILD_LOG(LogLevel::Debug, "Destructor called");
}

mozilla::ipc::IPCResult SpeechRecognitionChild::RecvOnRecognitionResult(
    const nsCString& aTranscript, const bool& aIsFinal) {
  SRCHILD_LOG(LogLevel::Info, "RecvOnRecognitionResult: '%s' (final=%s)",
              aTranscript.get(), aIsFinal ? "true" : "false");

  if (mResultCallback) {
    SRCHILD_LOG(LogLevel::Debug, "Invoking result callback");
    mResultCallback(aTranscript, aIsFinal);
  } else {
    SRCHILD_LOG(LogLevel::Warning, "Received result but no callback set");
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionChild::RecvOnRecognitionError(
    const nsCString& aError) {
  SRCHILD_LOG(LogLevel::Warning, "RecvOnRecognitionError: '%s'", aError.get());

  if (mErrorCallback) {
    SRCHILD_LOG(LogLevel::Debug, "Invoking error callback");
    mErrorCallback(aError);
  } else {
    SRCHILD_LOG(LogLevel::Warning, "Received error but no callback set");
  }
  return IPC_OK();
}

void SpeechRecognitionChild::ActorDestroy(ActorDestroyReason aReason) {
  SRCHILD_LOG(LogLevel::Info, "ActorDestroy called, reason=%d",
              static_cast<int>(aReason));

  // Clean up callbacks
  if (mResultCallback || mErrorCallback) {
    SRCHILD_LOG(LogLevel::Debug, "Clearing callbacks (result=%s, error=%s)",
                mResultCallback ? "set" : "null",
                mErrorCallback ? "set" : "null");
  }
  mResultCallback = nullptr;
  mErrorCallback = nullptr;
}

void SpeechRecognitionChild::SetResultCallback(
    RecognitionResultCallback&& aCallback) {
  SRCHILD_LOG(LogLevel::Debug, "SetResultCallback called");
  mResultCallback = std::move(aCallback);
}

void SpeechRecognitionChild::SetErrorCallback(
    RecognitionErrorCallback&& aCallback) {
  SRCHILD_LOG(LogLevel::Debug, "SetErrorCallback called");
  mErrorCallback = std::move(aCallback);
}

}  // namespace mozilla::ipc
