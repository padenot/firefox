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

namespace mozilla {
namespace ipc {

class HWInferenceManagerChild;

class SpeechRecognitionChild final : public PSpeechRecognitionChild {
 public:
  NS_INLINE_DECL_REFCOUNTING(SpeechRecognitionChild)

  using RecognitionResultCallback = std::function<void(const nsCString&, bool)>;
  using RecognitionErrorCallback = std::function<void(const nsCString&)>;

  explicit SpeechRecognitionChild(uint64_t aSessionId,
                                  HWInferenceManagerChild* aManager);

  void SetResultCallback(RecognitionResultCallback&& aCallback);
  void SetErrorCallback(RecognitionErrorCallback&& aCallback);

  mozilla::ipc::IPCResult RecvOnRecognitionResult(const nsCString& aTranscript,
                                                  const bool& aIsFinal);
  mozilla::ipc::IPCResult RecvOnRecognitionError(const nsCString& aError);

  void ActorDestroy(ActorDestroyReason aReason) override;

  uint64_t GetSessionId() const { return mSessionId; }

 private:
  ~SpeechRecognitionChild();

  uint64_t mSessionId;
  RefPtr<HWInferenceManagerChild> mManager;
  RecognitionResultCallback mResultCallback;
  RecognitionErrorCallback mErrorCallback;
};

}  // namespace ipc
}  // namespace mozilla

#endif  // mozilla_ipc_SpeechRecognitionChild_h
