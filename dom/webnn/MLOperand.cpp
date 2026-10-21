/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/MLOperand.h"

#include "mozilla/dom/MLGraphBuilder.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MLOperand, mBuilder)
NS_IMPL_CYCLE_COLLECTING_ADDREF(MLOperand)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MLOperand)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MLOperand)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

MLOperand::MLOperand(MLGraphBuilder* aBuilder, uint32_t aIndex, Kind aKind,
                     MLOperandDataType aDataType, nsTArray<uint32_t>&& aShape)
    : mBuilder(aBuilder),
      mIndex(aIndex),
      mKind(aKind),
      mDataType(aDataType),
      mShape(std::move(aShape)) {}

JSObject* MLOperand::WrapObject(JSContext* aCx,
                                JS::Handle<JSObject*> aGivenProto) {
  return MLOperand_Binding::Wrap(aCx, this, aGivenProto);
}

void MLOperand::GetShape(nsTArray<uint32_t>& aRetVal) const {
  aRetVal = mShape.Clone();
}

}  // namespace mozilla::dom
