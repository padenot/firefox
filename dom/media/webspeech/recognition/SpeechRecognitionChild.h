/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8  et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONCHILD_H_
#define DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONCHILD_H_

#include <functional>

#include "mozilla/PSpeechRecognitionChild.h"
#include "nsISupportsImpl.h"

namespace mozilla {

class HWInferenceManagerChild;

class SpeechRecognitionChild final : public PSpeechRecognitionChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SpeechRecognitionChild, override)
  using RecognitionResultCallback = std::function<void(const nsCString&, bool)>;
  using RecognitionErrorCallback = std::function<void(const nsCString&)>;
  using SpeechChangeCallback = std::function<void(bool)>;

  SpeechRecognitionChild();

  void SetResultCallback(RecognitionResultCallback&& aCallback);
  void SetErrorCallback(RecognitionErrorCallback&& aCallback);
  void SetSpeechChangeCallback(SpeechChangeCallback&& aCallback);

  mozilla::ipc::IPCResult RecvOnRecognitionResult(const nsCString& aTranscript,
                                                  const bool& aIsFinal);
  mozilla::ipc::IPCResult RecvOnRecognitionError(const nsCString& aError);
  mozilla::ipc::IPCResult RecvOnSpeechChange(const bool& aSpeechDetected);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  ~SpeechRecognitionChild();
  RecognitionResultCallback mResultCallback;
  RecognitionErrorCallback mErrorCallback;
  SpeechChangeCallback mSpeechChangeCallback;
};

}  // namespace mozilla

#endif // DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONCHILD_H_
