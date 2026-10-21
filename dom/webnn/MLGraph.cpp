/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/MLGraph.h"

#include "mozilla/dom/MLContext.h"
#include "mozilla/webnn/WebNNChild.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MLGraph, mContext)
NS_IMPL_CYCLE_COLLECTING_ADDREF(MLGraph)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MLGraph)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MLGraph)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

MLGraph::MLGraph(MLContext* aContext, uint32_t aId, OperandMap&& aInputs,
                 OperandMap&& aOutputs)
    : mContext(aContext),
      mId(aId),
      mInputs(std::move(aInputs)),
      mOutputs(std::move(aOutputs)) {}

MLGraph::~MLGraph() { Destroy(); }

JSObject* MLGraph::WrapObject(JSContext* aCx,
                              JS::Handle<JSObject*> aGivenProto) {
  return MLGraph_Binding::Wrap(aCx, this, aGivenProto);
}

void MLGraph::Destroy() {
  if (mDestroyed) {
    return;
  }
  mDestroyed = true;
  if (webnn::WebNNChild* actor = mContext ? mContext->Actor() : nullptr) {
    actor->SendDestroyGraph(mId);
  }
}

}  // namespace mozilla::dom
