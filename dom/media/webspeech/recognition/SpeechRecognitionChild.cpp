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
          ("[SRChild:%p] " fmt, this, ##__VA_ARGS__))

SpeechRecognitionChild::SpeechRecognitionChild(
    HWInferenceManagerChild* aManager)
    : mManager(aManager) {
  SRCHILD_LOG(LogLevel::Debug, "Constructor called with manager=%p", aManager);
  MOZ_ASSERT(mManager);
}

SpeechRecognitionChild::~SpeechRecognitionChild() {
  SRCHILD_LOG(LogLevel::Debug, "Destructor called");
}

void SpeechRecognitionChild::ActorDestroy(ActorDestroyReason aReason) {
  SRCHILD_LOG(LogLevel::Info, "ActorDestroy called, reason=%d",
              static_cast<int>(aReason));

  // Clean up callbacks
  if (mResultCallback || mErrorCallback || mSpeechChangeCallback) {
    SRCHILD_LOG(LogLevel::Debug, "Clearing callbacks (result=%s, error=%s, speechChange=%s)",
                mResultCallback ? "set" : "null",
                mErrorCallback ? "set" : "null",
                mSpeechChangeCallback ? "set" : "null");
  }
  mResultCallback = nullptr;
  mErrorCallback = nullptr;
  mSpeechChangeCallback = nullptr;
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

void SpeechRecognitionChild::SetSpeechChangeCallback(
    SpeechChangeCallback&& aCallback) {
  SRCHILD_LOG(LogLevel::Debug, "SetSpeechChangeCallback called");
  mSpeechChangeCallback = std::move(aCallback);
}

}  // namespace mozilla::ipc
