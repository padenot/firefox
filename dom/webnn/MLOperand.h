/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_MLOPERAND_H_
#define DOM_WEBNN_MLOPERAND_H_

#include "nsTArray.h"
#include "nsWrapperCache.h"

namespace mozilla::dom {

class MLGraphBuilder;
// Declared in WebNNBinding.h, which includes this file.
enum class MLOperandDataType : uint8_t;

class MLOperand final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MLOperand)

  enum class Kind { Input, Constant, Output };

  // aIndex is the index of this operand in the builder's operand list.
  MLOperand(MLGraphBuilder* aBuilder, uint32_t aIndex, Kind aKind,
            MLOperandDataType aDataType, nsTArray<uint32_t>&& aShape);

  MLGraphBuilder* GetParentObject() const { return mBuilder; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  MLOperandDataType DataType() const { return mDataType; }
  void GetShape(nsTArray<uint32_t>& aRetVal) const;

  MLGraphBuilder* Builder() const { return mBuilder; }
  uint32_t Index() const { return mIndex; }
  Kind GetKind() const { return mKind; }
  const nsTArray<uint32_t>& Shape() const { return mShape; }

 private:
  ~MLOperand() = default;

  RefPtr<MLGraphBuilder> mBuilder;
  const uint32_t mIndex;
  const Kind mKind;
  const MLOperandDataType mDataType;
  const nsTArray<uint32_t> mShape;
};

}  // namespace mozilla::dom

#endif  // DOM_WEBNN_MLOPERAND_H_
