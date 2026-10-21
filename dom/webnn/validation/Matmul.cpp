/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

using dom::MLOperandDataType;

bool ValidateMatmul(const dom::MLOperand& aA, const dom::MLOperand& aB,
                    const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (aA.DataType() != aB.DataType()) {
    aRv.ThrowTypeError("matmul: operands must have the same data type");
    return false;
  }
  if (aA.DataType() != MLOperandDataType::Float32 &&
      aA.DataType() != MLOperandDataType::Float16) {
    aRv.ThrowTypeError("matmul: unsupported data type");
    return false;
  }
  const nsTArray<uint32_t>& a = aA.Shape();
  const nsTArray<uint32_t>& b = aB.Shape();
  if (a.Length() < 2 || b.Length() < 2) {
    aRv.ThrowTypeError("matmul: operands must have a rank of at least 2");
    return false;
  }
  uint32_t m = a[a.Length() - 2];
  uint32_t k = a[a.Length() - 1];
  uint32_t n = b[b.Length() - 1];
  if (k != b[b.Length() - 2]) {
    aRv.ThrowTypeError("matmul: inner dimensions do not match");
    return false;
  }
  Maybe<nsTArray<uint32_t>> shape =
      BroadcastShapes(Span(a).To(a.Length() - 2), Span(b).To(b.Length() - 2));
  if (!shape) {
    aRv.ThrowTypeError("matmul: batch dimensions are not broadcastable");
    return false;
  }
  shape->AppendElement(m);
  shape->AppendElement(n);
  aOutputs.AppendElement(OperandDescriptor{aA.DataType(), shape.extract()});
  return true;
}

}  // namespace mozilla::webnn
