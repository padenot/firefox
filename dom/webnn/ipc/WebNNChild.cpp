/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/webnn/WebNNChild.h"

#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/MLContext.h"

namespace mozilla::webnn {

WebNNChild::WebNNChild(dom::MLContext* aContext) : mContext(aContext) {}

/* static */
already_AddRefed<WebNNChild> WebNNChild::Create(dom::MLContext* aContext) {
  MOZ_ASSERT(NS_IsMainThread());

#ifdef ANDROID
  return nullptr;
#else
  dom::ContentChild* contentChild = dom::ContentChild::GetSingleton();
  if (!contentChild) {
    return nullptr;
  }

  ipc::Endpoint<PWebNNParent> parentEndpoint;
  ipc::Endpoint<PWebNNChild> childEndpoint;
  if (NS_FAILED(PWebNN::CreateEndpoints(&parentEndpoint, &childEndpoint))) {
    return nullptr;
  }

  RefPtr<WebNNChild> child = new WebNNChild(aContext);
  if (!childEndpoint.Bind(child)) {
    return nullptr;
  }

  contentChild->SendAcquireHWInferenceProcess();
  child->mHoldsConnection = true;
  contentChild->SendCreateWebNN(std::move(parentEndpoint));
  return child.forget();
#endif
}

void WebNNChild::Detach() {
  mContext = nullptr;
  if (CanSend()) {
    Close();
  }
}

mozilla::ipc::IPCResult WebNNChild::RecvContextLost(const nsCString& aMessage) {
  if (mContext) {
    RefPtr<dom::MLContext> context = mContext;
    context->OnContextLost(aMessage);
  }
  return IPC_OK();
}

void WebNNChild::ActorDestroy(ActorDestroyReason aReason) {
#ifndef ANDROID
  if (mHoldsConnection) {
    mHoldsConnection = false;
    if (dom::ContentChild* contentChild = dom::ContentChild::GetSingleton()) {
      contentChild->SendReleaseHWInferenceConnection();
    }
  }
#endif
  if (mContext) {
    RefPtr<dom::MLContext> context = mContext;
    mContext = nullptr;
    context->OnContextLost("The WebNN process went away"_ns);
  }
}

}  // namespace mozilla::webnn
