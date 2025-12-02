/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_HWInferenceManagerParent_h
#define mozilla_ipc_HWInferenceManagerParent_h

#include "mozilla/ipc/PHWInferenceManagerParent.h"
#include "mozilla/dom/ipc/IdType.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/SpeechRecognitionParent.h"

namespace mozilla::hwinference {

using ipc::IPCResult;

class HWInferenceManagerParent final : public ipc::PHWInferenceManagerParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceManagerParent, override);

  static bool CreateForContent(Endpoint<PHWInferenceManagerParent>&& aEndpoint,
                               dom::ContentParentId aContentId);

  PSpeechRecognitionParent* AllocPSpeechRecognitionParent();
  bool DeallocPSpeechRecognitionParent(PSpeechRecognitionParent* aActor);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  explicit HWInferenceManagerParent(dom::ContentParentId aContentId);
  ~HWInferenceManagerParent() = default;

  const dom::ContentParentId mContentId;

  // Speech recognition actors (multiple allowed for Available/Install/Start)
  nsTArray<RefPtr<mozilla::SpeechRecognitionParent>> mSpeechSessions = {};
};

};  // namespace mozilla::hwinference

namespace mozilla::ipc {
using mozilla::hwinference::HWInferenceManagerParent;
}

#endif  // mozilla_ipc_HWInferenceManagerParent_h
