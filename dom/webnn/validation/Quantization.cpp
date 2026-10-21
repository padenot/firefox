/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

// Shape steps shared by quantizeLinear and dequantizeLinear (steps 7 to 10).
static bool CheckQuantizationShapes(const char* aOp,
                                    const dom::MLOperand& aInput,
                                    const dom::MLOperand& aScale,
                                    const dom::MLOperand& aZeroPoint,
                                    const nsAString& aLabel, ErrorResult& aRv) {
  size_t rank = aInput.Shape().Length();
  if (aScale.Shape().Length() != rank || aZeroPoint.Shape().Length() != rank) {
    ThrowOpTypeError(aRv, aLabel, aOp,
                     "scale and zeroPoint must have the input rank");
    return false;
  }
  if (aScale.Shape() != aZeroPoint.Shape()) {
    ThrowOpTypeError(aRv, aLabel, aOp, "scale and zeroPoint shapes must match");
    return false;
  }
  // zeroPoint has the same shape as scale, so one check covers both.
  if (!IsBlockwiseBroadcastable(aScale.Shape(), aInput.Shape())) {
    ThrowOpTypeError(aRv, aLabel, aOp,
                     "scale and zeroPoint must be blockwise broadcastable");
    return false;
  }
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-dequantizelinear
bool ValidateDequantizeLinear(const dom::MLOperand& aInput,
                              const dom::MLOperand& aScale,
                              const dom::MLOperand& aZeroPoint,
                              const dom::MLOperatorOptions& aOptions,
                              nsTArray<OperandDescriptor>& aOutputs,
                              ErrorResult& aRv) {
  if (!IsDataTypeIn(aInput.DataType(), kQuantizedTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "dequantizeLinear",
                     "unsupported input data type");
    return false;
  }
  if (!IsDataTypeIn(aScale.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "dequantizeLinear",
                     "unsupported scale data type");
    return false;
  }
  if (aZeroPoint.DataType() != aInput.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "dequantizeLinear",
                     "zeroPoint data type must match input");
    return false;
  }
  if (!CheckQuantizationShapes("dequantizeLinear", aInput, aScale, aZeroPoint,
                               aOptions.mLabel, aRv)) {
    return false;
  }
  return CreateOperandDescriptor(aScale.DataType(), aInput.Shape().Clone(),
                                 aOptions.mLabel, "dequantizeLinear", aOutputs,
                                 aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-quantizelinear
bool ValidateQuantizeLinear(const dom::MLOperand& aInput,
                            const dom::MLOperand& aScale,
                            const dom::MLOperand& aZeroPoint,
                            const dom::MLOperatorOptions& aOptions,
                            nsTArray<OperandDescriptor>& aOutputs,
                            ErrorResult& aRv) {
  if (!IsDataTypeIn(aInput.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "quantizeLinear",
                     "unsupported input data type");
    return false;
  }
  if (aScale.DataType() != aInput.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "quantizeLinear",
                     "scale data type must match input");
    return false;
  }
  if (!IsDataTypeIn(aZeroPoint.DataType(), kQuantizedTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "quantizeLinear",
                     "unsupported zeroPoint data type");
    return false;
  }
  if (!CheckQuantizationShapes("quantizeLinear", aInput, aScale, aZeroPoint,
                               aOptions.mLabel, aRv)) {
    return false;
  }
  return CreateOperandDescriptor(aZeroPoint.DataType(), aInput.Shape().Clone(),
                                 aOptions.mLabel, "quantizeLinear", aOutputs,
                                 aRv);
}

}  // namespace mozilla::webnn
