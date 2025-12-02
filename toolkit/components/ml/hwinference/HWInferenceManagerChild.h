/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_HWInferenceManagerChild_h
#define mozilla_ipc_HWInferenceManagerChild_h

#include "mozilla/ipc/PHWInferenceManagerChild.h"
#include "nsRefPtrHashtable.h"
#include "mozilla/StaticPtr.h"

namespace mozilla {
class SpeechRecognitionChild;
}

namespace mozilla::hwinference {

// Content process side
class HWInferenceManagerChild final : public ipc::PHWInferenceManagerChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceManagerChild, override);

  HWInferenceManagerChild() = default;

  static void OpenForProcess(Endpoint<PHWInferenceManagerChild>&& aEndpoint);

  static RefPtr<HWInferenceManagerChild> GetSingleton();

  void ActorDestroy(ActorDestroyReason aReason) override;

  ipc::PSpeechRecognitionChild* AllocPSpeechRecognitionChild();
  bool DeallocPSpeechRecognitionChild(ipc::PSpeechRecognitionChild* aActor);
  RefPtr<SpeechRecognitionChild> CreateSpeechRecognitionSession();

 private:
  ~HWInferenceManagerChild() = default;

  static StaticRefPtr<HWInferenceManagerChild> sSingleton;
  // Speech recognition actors
  nsTArray<RefPtr<SpeechRecognitionChild>> mSpeechSessions;
};

}  // namespace mozilla::hwinference

#endif  // mozilla_ipc_HWInferenceManagerChild_h
