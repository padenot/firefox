/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/MLTensor.h"

#include "mozilla/dom/MLContext.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/webnn/WebNNChild.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MLTensor, mContext, mPendingPromises)
NS_IMPL_CYCLE_COLLECTING_ADDREF(MLTensor)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MLTensor)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MLTensor)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

MLTensor::MLTensor(MLContext* aContext, uint32_t aId,
                   MLOperandDataType aDataType, nsTArray<uint32_t>&& aShape,
                   size_t aByteLength, bool aReadable, bool aWritable)
    : mContext(aContext),
      mId(aId),
      mDataType(aDataType),
      mShape(std::move(aShape)),
      mByteLength(aByteLength),
      mReadable(aReadable),
      mWritable(aWritable) {}

MLTensor::~MLTensor() { Destroy(); }

JSObject* MLTensor::WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) {
  return MLTensor_Binding::Wrap(aCx, this, aGivenProto);
}

void MLTensor::GetShape(nsTArray<uint32_t>& aRetVal) const {
  aRetVal = mShape.Clone();
}

void MLTensor::Destroy() {
  if (mDestroyed) {
    return;
  }
  mDestroyed = true;
  for (Promise* promise : std::exchange(mPendingPromises, {})) {
    promise->MaybeRejectWithInvalidStateError("The tensor is destroyed");
  }
  if (webnn::WebNNChild* actor = mContext ? mContext->Actor() : nullptr) {
    actor->SendDestroyTensor(mId);
  }
}

void MLTensor::AddPendingPromise(Promise* aPromise) {
  mPendingPromises.AppendElement(aPromise);
}

bool MLTensor::RemovePendingPromise(Promise* aPromise) {
  return mPendingPromises.RemoveElement(aPromise);
}

}  // namespace mozilla::dom
