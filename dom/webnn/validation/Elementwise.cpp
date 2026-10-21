/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

using dom::MLOperandDataType;

bool ValidateAdd(const dom::MLOperand& aA, const dom::MLOperand& aB,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (aA.DataType() != aB.DataType()) {
    aRv.ThrowTypeError("add: operands must have the same data type");
    return false;
  }
  Maybe<nsTArray<uint32_t>> shape = BroadcastShapes(aA.Shape(), aB.Shape());
  if (!shape) {
    aRv.ThrowTypeError("add: shapes are not broadcastable");
    return false;
  }
  aOutputs.AppendElement(OperandDescriptor{aA.DataType(), shape.extract()});
  return true;
}

bool ValidateRelu(const dom::MLOperand& aInput,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (aInput.DataType() != MLOperandDataType::Float32 &&
      aInput.DataType() != MLOperandDataType::Float16) {
    aRv.ThrowTypeError("relu: unsupported data type");
    return false;
  }
  aOutputs.AppendElement(
      OperandDescriptor{aInput.DataType(), aInput.Shape().Clone()});
  return true;
}

}  // namespace mozilla::webnn
