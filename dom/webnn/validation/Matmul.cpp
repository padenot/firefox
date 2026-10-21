/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-matmul
bool ValidateMatmul(const dom::MLOperand& aA, const dom::MLOperand& aB,
                    const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  const nsAString& label = aOptions.mLabel;
  if (!IsDataTypeIn(aA.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, label, "matmul", "unsupported data type");
    return false;
  }
  if (aB.DataType() != aA.DataType()) {
    ThrowOpTypeError(aRv, label, "matmul",
                     "operands must have the same data type");
    return false;
  }
  const nsTArray<uint32_t>& a = aA.Shape();
  const nsTArray<uint32_t>& b = aB.Shape();
  if (a.Length() < 2 || b.Length() < 2) {
    ThrowOpTypeError(aRv, label, "matmul",
                     "operands must have a rank of at least 2");
    return false;
  }
  uint32_t m = a[a.Length() - 2];
  uint32_t k = a[a.Length() - 1];
  uint32_t n = b[b.Length() - 1];
  if (k != b[b.Length() - 2]) {
    ThrowOpTypeError(aRv, label, "matmul", "inner dimensions do not match");
    return false;
  }
  Maybe<nsTArray<uint32_t>> shape = BidirectionallyBroadcastShapes(
      Span(a).To(a.Length() - 2), Span(b).To(b.Length() - 2));
  if (!shape) {
    ThrowOpTypeError(aRv, label, "matmul",
                     "batch dimensions are not broadcastable");
    return false;
  }
  shape->AppendElement(m);
  shape->AppendElement(n);
  return CreateOperandDescriptor(aA.DataType(), shape.extract(), label,
                                 "matmul", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-gemm
bool ValidateGemm(const dom::MLOperand& aA, const dom::MLOperand& aB,
                  const dom::MLGemmOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!IsDataTypeIn(aA.DataType(), kFloatTypes) ||
      aB.DataType() != aA.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gemm", "unsupported data types");
    return false;
  }
  if (aA.Shape().Length() != 2 || aB.Shape().Length() != 2) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gemm",
                     "operands must be 2-D tensors");
    return false;
  }
  nsTArray<uint32_t> shapeA = aA.Shape().Clone();
  nsTArray<uint32_t> shapeB = aB.Shape().Clone();
  if (aOptions.mATranspose) {
    shapeA.Reverse();
  }
  if (aOptions.mBTranspose) {
    shapeB.Reverse();
  }
  if (shapeA[1] != shapeB[0]) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gemm",
                     "inner dimensions do not match");
    return false;
  }
  nsTArray<uint32_t> outputShape = {shapeA[0], shapeB[1]};
  if (aOptions.mC.WasPassed()) {
    const dom::MLOperand& c = aOptions.mC.Value();
    if (!IsUnidirectionallyBroadcastable(c.Shape(), outputShape)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "gemm",
                       "c is not broadcastable to the output shape");
      return false;
    }
    if (c.DataType() != aA.DataType()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "gemm",
                       "c data type must match a");
      return false;
    }
  }
  return CreateOperandDescriptor(aA.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "gemm", aOutputs, aRv);
}

}  // namespace mozilla::webnn
