/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_MLGRAPHBUILDER_H_
#define DOM_WEBNN_MLGRAPHBUILDER_H_

#include "mozilla/dom/BufferSourceBinding.h"
#include "mozilla/dom/MLOperand.h"
#include "mozilla/dom/Record.h"
#include "mozilla/dom/TypedArray.h"
#include "mozilla/dom/WebNNBinding.h"
#include "mozilla/ipc/BigBuffer.h"
#include "mozilla/webnn/WebNNTypes.h"
#include "mozilla/webnn/WebNNValidation.h"
#include "nsTHashSet.h"
#include "nsWrapperCache.h"

namespace mozilla::dom {

class MLContext;
class MLTensor;
class Promise;

// Validates operations and computes their output descriptors synchronously, and
// records them. build() sends the recorded graph to the HWInference process.
class MLGraphBuilder final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS_FINAL
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MLGraphBuilder)

  static already_AddRefed<MLGraphBuilder> Constructor(
      const GlobalObject& aGlobal, MLContext& aContext, ErrorResult& aRv);

  MLContext* GetParentObject() const { return mContext; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<MLOperand> Input(const nsAString& aName,
                                    const MLOperandDescriptor& aDescriptor,
                                    ErrorResult& aRv);
  already_AddRefed<MLOperand> Constant(
      const MLOperandDescriptor& aDescriptor,
      const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aBuffer,
      ErrorResult& aRv);
  already_AddRefed<MLOperand> Constant(MLOperandDataType aDataType,
                                       double aValue, ErrorResult& aRv);
  already_AddRefed<MLOperand> Constant(MLTensor& aTensor, ErrorResult& aRv);
  already_AddRefed<Promise> Build(
      const Record<nsString, OwningNonNull<MLOperand>>& aOutputs,
      ErrorResult& aRv);

  // IPDL operand indices.
  static Maybe<uint32_t> OptionalIndex(
      const Optional<OwningNonNull<MLOperand>>& aOperand);
  static nsTArray<uint32_t> Indices(
      const Sequence<OwningNonNull<MLOperand>>& aOperands);

  // Operators, generated from WebNN.webidl.
#include "mozilla/dom/MLGraphBuilderOps.h"

 private:
  explicit MLGraphBuilder(MLContext* aContext);
  ~MLGraphBuilder() = default;

  bool CheckState(ErrorResult& aRv) const;
  bool CheckOperand(const MLOperand& aOperand, ErrorResult& aRv) const;
  bool CheckOperand(const Optional<OwningNonNull<MLOperand>>& aOperand,
                    ErrorResult& aRv) const;
  bool CheckOperands(const Sequence<OwningNonNull<MLOperand>>& aOperands,
                     ErrorResult& aRv) const;
  nsTArray<RefPtr<MLOperand>> AddOutputs(
      nsTArray<webnn::OperandDescriptor>&& aDescriptors);
  static nsTArray<uint32_t> OutputIndices(
      const nsTArray<RefPtr<MLOperand>>& aOperands);
  already_AddRefed<MLOperand> AddOperand(MLOperand::Kind aKind,
                                         MLOperandDataType aDataType,
                                         nsTArray<uint32_t>&& aShape);
  already_AddRefed<MLOperand> AddConstant(MLOperandDataType aDataType,
                                          nsTArray<uint32_t>&& aShape,
                                          Span<const uint8_t> aData);

  RefPtr<MLContext> mContext;
  nsTArray<webnn::WebNNOperandDesc> mOperands;
  nsTArray<webnn::WebNNOperation> mOperations;
  nsTArray<mozilla::ipc::BigBuffer> mConstants;
  nsTHashSet<nsCString> mInputNames;
  bool mBuilt = false;
};

}  // namespace mozilla::dom

#endif  // DOM_WEBNN_MLGRAPHBUILDER_H_
