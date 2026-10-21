/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

using dom::MLOperand;

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-reduce
static bool ValidateReduction(
    const char* aOp, const MLOperand& aInput,
    const dom::MLReduceOptions& aOptions,
    const Maybe<Span<const MLOperandDataType>>& aAllowedDataTypes,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (aAllowedDataTypes &&
      !IsDataTypeIn(aInput.DataType(), *aAllowedDataTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "unsupported data type");
    return false;
  }
  Maybe<Span<const uint32_t>> axes;
  if (aOptions.mAxes.WasPassed()) {
    axes.emplace(aOptions.mAxes.Value());
  }
  Maybe<nsTArray<uint32_t>> shape = CalculateReductionOutputSizes(
      aInput.Shape(), axes, aOptions.mKeepDimensions);
  if (!shape) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "axes must be unique and less than the input rank");
    return false;
  }
  return CreateOperandDescriptor(aInput.DataType(), shape.extract(),
                                 aOptions.mLabel, aOp, aOutputs, aRv);
}

#define WEBNN_REDUCTION(Name, op, allowed)                                  \
  bool Validate##Name(                                                      \
      const MLOperand& aInput, const dom::MLReduceOptions& aOptions,        \
      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {            \
    return ValidateReduction(op, aInput, aOptions, allowed, aOutputs, aRv); \
  }

#define WEBNN_FLOAT Some(Span<const MLOperandDataType>(kFloatTypes))
#define WEBNN_FLOAT_WIDE_INT \
  Some(Span<const MLOperandDataType>(kFloatAndWideIntTypes))

WEBNN_REDUCTION(ReduceL1, "reduceL1", WEBNN_FLOAT_WIDE_INT)
WEBNN_REDUCTION(ReduceL2, "reduceL2", WEBNN_FLOAT)
WEBNN_REDUCTION(ReduceLogSum, "reduceLogSum", WEBNN_FLOAT)
WEBNN_REDUCTION(ReduceLogSumExp, "reduceLogSumExp", WEBNN_FLOAT)
WEBNN_REDUCTION(ReduceMax, "reduceMax", Nothing())
WEBNN_REDUCTION(ReduceMean, "reduceMean", WEBNN_FLOAT)
WEBNN_REDUCTION(ReduceMin, "reduceMin", Nothing())
WEBNN_REDUCTION(ReduceProduct, "reduceProduct", WEBNN_FLOAT_WIDE_INT)
WEBNN_REDUCTION(ReduceSum, "reduceSum", WEBNN_FLOAT_WIDE_INT)
WEBNN_REDUCTION(ReduceSumSquare, "reduceSumSquare", WEBNN_FLOAT_WIDE_INT)

#undef WEBNN_FLOAT
#undef WEBNN_FLOAT_WIDE_INT
#undef WEBNN_REDUCTION

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-argminmax
static bool ValidateArgMinMax(const char* aOp, const MLOperand& aInput,
                              uint32_t aAxis,
                              const dom::MLArgMinMaxOptions& aOptions,
                              nsTArray<OperandDescriptor>& aOutputs,
                              ErrorResult& aRv) {
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  if (aAxis >= inputShape.Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "axis must be less than the input rank");
    return false;
  }
  MLOperandDataType outputDataType = aOptions.mOutputDataType;
  if (outputDataType != MLOperandDataType::Int32 &&
      outputDataType != MLOperandDataType::Int64) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "outputDataType must be int32 or int64");
    return false;
  }
  if (outputDataType == MLOperandDataType::Int32 &&
      inputShape[aAxis] > uint32_t(INT32_MAX)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "axis dimension is too large for outputDataType");
    return false;
  }
  Maybe<nsTArray<uint32_t>> shape = CalculateReductionOutputSizes(
      inputShape, Some(Span<const uint32_t>(&aAxis, 1)),
      aOptions.mKeepDimensions);
  if (!shape) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "invalid axis");
    return false;
  }
  return CreateOperandDescriptor(outputDataType, shape.extract(),
                                 aOptions.mLabel, aOp, aOutputs, aRv);
}

bool ValidateArgMin(const MLOperand& aInput, uint32_t aAxis,
                    const dom::MLArgMinMaxOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  return ValidateArgMinMax("argMin", aInput, aAxis, aOptions, aOutputs, aRv);
}

bool ValidateArgMax(const MLOperand& aInput, uint32_t aAxis,
                    const dom::MLArgMinMaxOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  return ValidateArgMinMax("argMax", aInput, aAxis, aOptions, aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-cumulativesum
bool ValidateCumulativeSum(const MLOperand& aInput, uint32_t aAxis,
                           const dom::MLCumulativeSumOptions& aOptions,
                           nsTArray<OperandDescriptor>& aOutputs,
                           ErrorResult& aRv) {
  if (!IsDataTypeIn(aInput.DataType(), kFloatAndWideIntTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "cumulativeSum",
                     "unsupported data type");
    return false;
  }
  if (aAxis >= aInput.Shape().Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "cumulativeSum",
                     "axis must be less than the input rank");
    return false;
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

}  // namespace mozilla::webnn
