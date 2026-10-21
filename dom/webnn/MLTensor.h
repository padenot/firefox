/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_MLTENSOR_H_
#define DOM_WEBNN_MLTENSOR_H_

#include "mozilla/dom/WebNNBinding.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"

namespace mozilla::dom {

class MLContext;

class MLTensor final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MLTensor)

  MLTensor(MLContext* aContext, uint32_t aId, MLOperandDataType aDataType,
           nsTArray<uint32_t>&& aShape, size_t aByteLength, bool aReadable,
           bool aWritable);

  MLContext* GetParentObject() const { return mContext; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  MLOperandDataType DataType() const { return mDataType; }
  void GetShape(nsTArray<uint32_t>& aRetVal) const;
  bool Readable() const { return mReadable; }
  bool Writable() const { return mWritable; }
  bool Constant() const { return false; }
  void Destroy();

  MLContext* Context() const { return mContext; }
  uint32_t Id() const { return mId; }
  size_t ByteLength() const { return mByteLength; }
  const nsTArray<uint32_t>& Shape() const { return mShape; }
  bool IsDestroyed() const { return mDestroyed; }

 private:
  ~MLTensor();

  RefPtr<MLContext> mContext;
  const uint32_t mId;
  const MLOperandDataType mDataType;
  const nsTArray<uint32_t> mShape;
  const size_t mByteLength;
  const bool mReadable;
  const bool mWritable;
  bool mDestroyed = false;
};

}  // namespace mozilla::dom

#endif  // DOM_WEBNN_MLTENSOR_H_
