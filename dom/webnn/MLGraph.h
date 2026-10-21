/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_MLGRAPH_H_
#define DOM_WEBNN_MLGRAPH_H_

#include "mozilla/dom/WebNNBinding.h"
#include "nsTHashMap.h"
#include "nsWrapperCache.h"

namespace mozilla::dom {

class MLContext;

class MLGraph final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MLGraph)

  struct OperandInfo {
    MLOperandDataType mDataType;
    nsTArray<uint32_t> mShape;
  };
  using OperandMap = nsTHashMap<nsCStringHashKey, OperandInfo>;

  MLGraph(MLContext* aContext, uint32_t aId, OperandMap&& aInputs,
          OperandMap&& aOutputs);

  MLContext* GetParentObject() const { return mContext; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  void Destroy();

  MLContext* Context() const { return mContext; }
  uint32_t Id() const { return mId; }
  bool IsDestroyed() const { return mDestroyed; }
  const OperandMap& Inputs() const { return mInputs; }
  const OperandMap& Outputs() const { return mOutputs; }

 private:
  ~MLGraph();

  RefPtr<MLContext> mContext;
  const uint32_t mId;
  const OperandMap mInputs;
  const OperandMap mOutputs;
  bool mDestroyed = false;
};

}  // namespace mozilla::dom

#endif  // DOM_WEBNN_MLGRAPH_H_
