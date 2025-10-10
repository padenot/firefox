/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionParent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <ratio>
#include <thread>

#include "mozIRemoteLazyInputStream.h"
#include "mozilla/Logging.h"
#include "mozilla/Mutex.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/HWInferenceChild.h"
#include "mozilla/ipc/PHWInference.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/llama/LlamaRuntimeLinker.h"
#include "nsDebug.h"
#include "nsGkAtoms.h"
#include "nsIFileStreams.h"
#include "nsNetUtil.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "prio.h"
#include "private/pprio.h"
#include "whisper.h"

#ifdef XP_WIN
#  include <fcntl.h>
#endif

#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"

namespace mozilla::ipc {

// Static initialization
StaticRefPtr<SpeechRecognitionParent> SpeechRecognitionParent::sActiveSession;
StaticMutex SpeechRecognitionParent::sSessionMutex;

static LazyLogModule gSpeechRecognitionParentLog("SpeechRecognitionParent");
#define LOGV(fmt, ...)                                             \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Verbose, fmt, \
              ##__VA_ARGS__)
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Error, fmt, ##__VA_ARGS__)
SpeechRecognitionParent::ModelIdentifier
SpeechRecognitionParent::LanguagesToModelIdentifier(
    const nsTArray<nsCString>& aLanguages) {
  MOZ_ASSERT(!aLanguages.IsEmpty());

  // Map languages to model names for speech recognition
  // en goes to ggml-small.en
  // all other languages fall back to large turbo v3
  nsCString modelName;
  nsCString fileName;
  nsCString revision = "main"_ns;

  // todo while loop, support requesting multiple languages
  const nsCString& firstLang = aLanguages[0];
  if (firstLang.EqualsLiteral("en") || firstLang.EqualsLiteral("en-US")) {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-small.en.bin"_ns;
  } else {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-large-v3-turbo-q8_0.bin"_ns;
  }

  return {modelName, fileName, revision};
}

nsCString SpeechRecognitionParent::ModelIdentifier::ToString() const {
  return nsFmtCString("{}/{}/{}", mModelName.get(), mFileName.get(),
                      mRevision.get());
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvIsModelAvailable(
    const nsTArray<nsCString>& aLanguages,
    IsModelAvailableResolver&& aResolver) {
  LOGD("{} RecvIsModelAvailable called for languages: {}", __func__,
       fmt::join(aLanguages, ", "));

  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);

  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} No UtilityProcessChild available", __func__);
    aResolver(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available", __func__);
    aResolver(false);
    return IPC_OK();
  }

  LOGD(
      "{} Sending model availability request to main process, {} "
      "mapped to model={}",
      __func__, fmt::join(aLanguages, ", "), modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendIsModelAvailable(modelIdentifier.mModelName,
                             modelIdentifier.mRevision,
                             modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}, aResolver](bool aAvailable) mutable {
            LOGD("Sending response back to content process: available={}",
                 aAvailable ? "true" : "false");
            aResolver(aAvailable);
          },
          [self = RefPtr{this},
           aResolver](ResponseRejectReason aReason) mutable {
            LOGE("{} IPC call to main process failed: {}", __func__,
                 static_cast<int>(aReason));
            aResolver(false);
          });

  return IPC_OK();
}
SpeechRecognitionParent::SpeechRecognitionParent()
    : mLock("SpeechRecognitionLock") {}

SpeechRecognitionParent::~SpeechRecognitionParent() {
  LOGD("{}", __func__);

  // Clear active session if this was it
  {
    StaticMutexAutoLock lock(sSessionMutex);
    if (sActiveSession == this) {
      LOGD("Clearing active session in destructor");
      sActiveSession = nullptr;
    }
  }
}

void SpeechRecognitionParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("{} ActorDestroy called", __func__);
}

}  // namespace mozilla::ipc

#undef LOGV
#undef LOGD
#undef LOGE
