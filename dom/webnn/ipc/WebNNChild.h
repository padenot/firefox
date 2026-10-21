/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_IPC_WEBNNCHILD_H_
#define DOM_WEBNN_IPC_WEBNNCHILD_H_

#include "mozilla/webnn/PWebNNChild.h"

namespace mozilla::dom {
class MLContext;
}

namespace mozilla::webnn {

// Content process side of PWebNN, owned by an MLContext. Bound on the main
// thread, and holds a connection to the HWInference process while alive.
class WebNNChild final : public PWebNNChild {
 public:
  NS_INLINE_DECL_REFCOUNTING(WebNNChild, override)

  // Asks the parent process to connect a new PWebNN actor to the HWInference
  // process. Returns null if the endpoints could not be created.
  static already_AddRefed<WebNNChild> Create(dom::MLContext* aContext);

  // Called when the owning MLContext goes away.
  void Detach();

  mozilla::ipc::IPCResult RecvContextLost(const nsCString& aMessage);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  explicit WebNNChild(dom::MLContext* aContext);
  ~WebNNChild() = default;

  // Weak, cleared by Detach().
  dom::MLContext* mContext;
  bool mHoldsConnection = false;
};

}  // namespace mozilla::webnn

#endif  // DOM_WEBNN_IPC_WEBNNCHILD_H_
