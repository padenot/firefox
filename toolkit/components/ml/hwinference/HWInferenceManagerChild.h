/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_HWInferenceManagerChild_h
#define mozilla_ipc_HWInferenceManagerChild_h

#include "mozilla/ipc/PHWInferenceManagerChild.h"
#include "nsRefPtrHashtable.h"

namespace mozilla {
namespace ipc {

class SpeechRecognitionChild;

class HWInferenceManagerChild final : public PHWInferenceManagerChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceManagerChild, override);

  HWInferenceManagerChild();

  // Static method to open and bind the endpoint properly
  static void OpenForProcess(Endpoint<PHWInferenceManagerChild>&& aEndpoint);

  // Static method to get the singleton instance (may return null if not yet
  // opened)
  static RefPtr<HWInferenceManagerChild> GetSingleton();

  void ActorDestroy(ActorDestroyReason aReason) override;

  // PSpeechRecognition management
  PSpeechRecognitionChild* AllocPSpeechRecognitionChild(
      const uint64_t& aSessionId);
  bool DeallocPSpeechRecognitionChild(PSpeechRecognitionChild* aActor);

  // Create a new speech recognition session
  RefPtr<SpeechRecognitionChild> CreateSpeechRecognitionSession(
      uint64_t aSessionId);

 private:
  ~HWInferenceManagerChild() = default;

  static RefPtr<HWInferenceManagerChild> sSingleton;

  // Map of session IDs to speech recognition actors
  nsRefPtrHashtable<nsUint64HashKey, SpeechRecognitionChild> mSpeechSessions;
};

}  // namespace ipc
}  // namespace mozilla

#endif  // mozilla_ipc_HWInferenceManagerChild_h
