/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNUtils.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

// True if aOperand has the shape [aSize].
static bool HasShape1d(const dom::MLOperand& aOperand, uint32_t aSize) {
  return aOperand.Shape().Length() == 1 && aOperand.Shape()[0] == aSize;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-batchnorm
bool ValidateBatchNormalization(
    const dom::MLOperand& aInput, const dom::MLOperand& aMean,
    const dom::MLOperand& aVariance,
    const dom::MLBatchNormalizationOptions& aOptions,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  dom::MLOperandDataType type = aInput.DataType();
  if (!IsDataTypeIn(type, kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                     "unsupported input data type");
    return false;
  }
  if (aOptions.mAxis >= aInput.Shape().Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                     "axis is out of range");
    return false;
  }
  uint32_t size = aInput.Shape()[aOptions.mAxis];
  if (aMean.DataType() != type) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                     "mean data type must match input");
    return false;
  }
  if (!HasShape1d(aMean, size)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                     "invalid mean shape");
    return false;
  }
  if (aVariance.DataType() != type) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                     "variance data type must match input");
    return false;
  }
  if (!HasShape1d(aVariance, size)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                     "invalid variance shape");
    return false;
  }
  if (aOptions.mScale.WasPassed()) {
    const dom::MLOperand& scale = aOptions.mScale.Value();
    if (scale.DataType() != type) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                       "scale data type must match input");
      return false;
    }
    if (!HasShape1d(scale, size)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                       "invalid scale shape");
      return false;
    }
  }
  if (aOptions.mBias.WasPassed()) {
    const dom::MLOperand& bias = aOptions.mBias.Value();
    if (bias.DataType() != type) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                       "bias data type must match input");
      return false;
    }
    if (!HasShape1d(bias, size)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "batchNormalization",
                       "invalid bias shape");
      return false;
    }
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-instancenormalization
bool ValidateInstanceNormalization(
    const dom::MLOperand& aInput,
    const dom::MLInstanceNormalizationOptions& aOptions,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  dom::MLOperandDataType type = aInput.DataType();
  if (!IsDataTypeIn(type, kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "instanceNormalization",
                     "unsupported input data type");
    return false;
  }
  if (aInput.Shape().Length() != 4) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "instanceNormalization",
                     "input must be a 4-D tensor");
    return false;
  }
  size_t axis = aOptions.mLayout == dom::MLInputOperandLayout::Nchw ? 1 : 3;
  uint32_t size = aInput.Shape()[axis];
  if (aOptions.mScale.WasPassed()) {
    const dom::MLOperand& scale = aOptions.mScale.Value();
    if (scale.DataType() != type) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "instanceNormalization",
                       "scale data type must match input");
      return false;
    }
    if (!HasShape1d(scale, size)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "instanceNormalization",
                       "invalid scale shape");
      return false;
    }
  }
  if (aOptions.mBias.WasPassed()) {
    const dom::MLOperand& bias = aOptions.mBias.Value();
    if (bias.DataType() != type) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "instanceNormalization",
                       "bias data type must match input");
      return false;
    }
    if (!HasShape1d(bias, size)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "instanceNormalization",
                       "invalid bias shape");
      return false;
    }
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-layernorm
bool ValidateLayerNormalization(
    const dom::MLOperand& aInput,
    const dom::MLLayerNormalizationOptions& aOptions,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  dom::MLOperandDataType type = aInput.DataType();
  if (!IsDataTypeIn(type, kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                     "unsupported input data type");
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  nsTArray<uint32_t> axes;
  if (!aOptions.mAxes.WasPassed()) {
    for (uint32_t i = 1; i < inputShape.Length(); i++) {
      axes.AppendElement(i);
    }
  } else {
    axes = ToShape(aOptions.mAxes.Value());
    if (!AreValidAxes(axes, inputShape.Length())) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                       "invalid axes");
      return false;
    }
  }
  const dom::MLOperand* scale =
      aOptions.mScale.WasPassed() ? &aOptions.mScale.Value() : nullptr;
  const dom::MLOperand* bias =
      aOptions.mBias.WasPassed() ? &aOptions.mBias.Value() : nullptr;
  if (scale) {
    if (scale->DataType() != type) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                       "scale data type must match input");
      return false;
    }
    if (scale->Shape().Length() != axes.Length()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                       "invalid scale rank");
      return false;
    }
  }
  if (bias) {
    if (bias->DataType() != type) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                       "bias data type must match input");
      return false;
    }
    if (bias->Shape().Length() != axes.Length()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                       "invalid bias rank");
      return false;
    }
  }
  for (size_t i = 0; i < axes.Length(); i++) {
    uint32_t size = inputShape[axes[i]];
    if (scale && scale->Shape()[i] != size) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                       "invalid scale shape");
      return false;
    }
    if (bias && bias->Shape()[i] != size) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "layerNormalization",
                       "invalid bias shape");
      return false;
    }
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

}  // namespace mozilla::webnn
