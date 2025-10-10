/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_SpeechRecognitionChild_h
#define mozilla_ipc_SpeechRecognitionChild_h

#include <functional>

#include "mozilla/ipc/PSpeechRecognitionChild.h"
#include "nsISupportsImpl.h"

namespace mozilla::ipc {

class HWInferenceManagerChild;

class SpeechRecognitionChild final : public PSpeechRecognitionChild {
 public:
  NS_INLINE_DECL_REFCOUNTING(SpeechRecognitionChild, override)
  using RecognitionResultCallback = std::function<void(const nsCString&, bool)>;
  using RecognitionErrorCallback = std::function<void(const nsCString&)>;
  using SpeechChangeCallback = std::function<void(bool)>;

  SpeechRecognitionChild();

  void SetResultCallback(RecognitionResultCallback&& aCallback);
  void SetErrorCallback(RecognitionErrorCallback&& aCallback);
  void SetSpeechChangeCallback(SpeechChangeCallback&& aCallback);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  ~SpeechRecognitionChild();
  RecognitionResultCallback mResultCallback;
  RecognitionErrorCallback mErrorCallback;
  SpeechChangeCallback mSpeechChangeCallback;
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_SpeechRecognitionChild_h
