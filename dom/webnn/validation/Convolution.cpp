/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <array>

#include "ValidationUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

using dom::MLInputOperandLayout;

template <size_t N>
static bool ContainsZero(const std::array<uint32_t, N>& aList) {
  for (uint32_t v : aList) {
    if (!v) {
      return true;
    }
  }
  return false;
}

// The spec's "calculate conv output size" returns a real number; callers
// round it. This returns floor() and ceil() of it, or Nothing() if the
// computation overflows int64 (in which case the result is never a valid
// dimension).
struct ConvOutputSize {
  int64_t mFloor;
  int64_t mCeil;
};

// https://webmachinelearning.github.io/webnn/#calculate-conv-output-size
static Maybe<ConvOutputSize> CalculateConvOutputSize(
    uint32_t aInputSize, uint32_t aFilterSize, uint32_t aBeginningPadding,
    uint32_t aEndingPadding, uint32_t aStride, uint32_t aDilation) {
  CheckedInt<int64_t> effectiveFilterSize =
      (CheckedInt<int64_t>(aFilterSize) - 1) * aDilation + 1;
  CheckedInt<int64_t> numerator = CheckedInt<int64_t>(aInputSize) -
                                  effectiveFilterSize + aBeginningPadding +
                                  aEndingPadding;
  if (!numerator.isValid()) {
    return Nothing();
  }
  int64_t n = numerator.value();
  int64_t quotient = n / aStride;
  bool exact = n % aStride == 0;
  int64_t floorQuotient = (!exact && n < 0) ? quotient - 1 : quotient;
  int64_t ceilQuotient = (!exact && n > 0) ? quotient + 1 : quotient;
  return Some(ConvOutputSize{floorQuotient + 1, ceilQuotient + 1});
}

// https://webmachinelearning.github.io/webnn/#calculate-conv2d-output-sizes
static Maybe<std::array<ConvOutputSize, 2>> CalculateConv2dOutputSizes(
    uint32_t aInputHeight, uint32_t aInputWidth, uint32_t aFilterHeight,
    uint32_t aFilterWidth, const std::array<uint32_t, 4>& aPadding,
    const std::array<uint32_t, 2>& aStrides,
    const std::array<uint32_t, 2>& aDilations) {
  Maybe<ConvOutputSize> height =
      CalculateConvOutputSize(aInputHeight, aFilterHeight, aPadding[0],
                              aPadding[1], aStrides[0], aDilations[0]);
  Maybe<ConvOutputSize> width =
      CalculateConvOutputSize(aInputWidth, aFilterWidth, aPadding[2],
                              aPadding[3], aStrides[1], aDilations[1]);
  if (!height || !width) {
    return Nothing();
  }
  return Some(std::array<ConvOutputSize, 2>{*height, *width});
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-conv2d
bool ValidateConv2d(const dom::MLOperand& aInput, const dom::MLOperand& aFilter,
                    const dom::MLConv2dOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!IsDataTypeIn(aInput.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "unsupported input data type");
    return false;
  }
  if (aInput.Shape().Length() != 4) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "input must be a 4-D tensor");
    return false;
  }
  if (aFilter.Shape().Length() != 4) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "filter must be a 4-D tensor");
    return false;
  }
  if (aFilter.DataType() != aInput.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "filter data type must match input");
    return false;
  }
  std::array<uint32_t, 4> padding;
  if (!ResolveListOption(aOptions.mPadding, 0, padding)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "padding must have 4 items");
    return false;
  }
  std::array<uint32_t, 2> strides;
  if (!ResolveListOption(aOptions.mStrides, 1, strides)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "strides must have 2 items");
    return false;
  }
  if (ContainsZero(strides)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "strides must be positive");
    return false;
  }
  std::array<uint32_t, 2> dilations;
  if (!ResolveListOption(aOptions.mDilations, 1, dilations)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "dilations must have 2 items");
    return false;
  }
  if (ContainsZero(dilations)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "dilations must be positive");
    return false;
  }
  uint32_t groups = aOptions.mGroups;
  if (!groups) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d", "groups must be positive");
    return false;
  }

  const nsTArray<uint32_t>& in = aInput.Shape();
  uint32_t batches = in[0];
  uint32_t inputChannels = 0, inputHeight = 0, inputWidth = 0;
  switch (aOptions.mInputLayout) {
    case MLInputOperandLayout::Nchw:
      inputChannels = in[1];
      inputHeight = in[2];
      inputWidth = in[3];
      break;
    case MLInputOperandLayout::Nhwc:
      inputHeight = in[1];
      inputWidth = in[2];
      inputChannels = in[3];
      break;
  }

  const nsTArray<uint32_t>& f = aFilter.Shape();
  uint32_t outputChannels = 0, filterInputChannels = 0, filterHeight = 0,
           filterWidth = 0;
  switch (aOptions.mFilterLayout) {
    case dom::MLConv2dFilterOperandLayout::Hwio:
      filterHeight = f[0];
      filterWidth = f[1];
      filterInputChannels = f[2];
      outputChannels = f[3];
      break;
    case dom::MLConv2dFilterOperandLayout::Ohwi:
      outputChannels = f[0];
      filterHeight = f[1];
      filterWidth = f[2];
      filterInputChannels = f[3];
      break;
    case dom::MLConv2dFilterOperandLayout::Ihwo:
      filterInputChannels = f[0];
      filterHeight = f[1];
      filterWidth = f[2];
      outputChannels = f[3];
      break;
    case dom::MLConv2dFilterOperandLayout::Oihw:
      outputChannels = f[0];
      filterInputChannels = f[1];
      filterHeight = f[2];
      filterWidth = f[3];
      break;
  }

  if (inputChannels % groups) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "input channels must be divisible by groups");
    return false;
  }
  if (inputChannels / groups != filterInputChannels) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "filter input channels must be input channels / groups");
    return false;
  }
  if (outputChannels % groups) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                     "output channels must be divisible by groups");
    return false;
  }
  if (aOptions.mBias.WasPassed()) {
    const dom::MLOperand& bias = aOptions.mBias.Value();
    if (bias.Shape().Length() != 1 || bias.Shape()[0] != outputChannels) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                       "bias shape must be [outputChannels]");
      return false;
    }
    if (bias.DataType() != aInput.DataType()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d",
                       "bias data type must match input");
      return false;
    }
  }

  Maybe<std::array<ConvOutputSize, 2>> sizes =
      CalculateConv2dOutputSizes(inputHeight, inputWidth, filterHeight,
                                 filterWidth, padding, strides, dilations);
  if (!sizes || !IsValidDimension((*sizes)[0].mFloor) ||
      !IsValidDimension((*sizes)[1].mFloor)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "conv2d", "invalid output size");
    return false;
  }
  uint32_t outputHeight = uint32_t((*sizes)[0].mFloor);
  uint32_t outputWidth = uint32_t((*sizes)[1].mFloor);

  nsTArray<uint32_t> outputShape;
  switch (aOptions.mInputLayout) {
    case MLInputOperandLayout::Nchw:
      outputShape = nsTArray<uint32_t>{batches, outputChannels, outputHeight,
                                       outputWidth};
      break;
    case MLInputOperandLayout::Nhwc:
      outputShape = nsTArray<uint32_t>{batches, outputHeight, outputWidth,
                                       outputChannels};
      break;
  }
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "conv2d", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#calculate-convtranspose-output-size
static CheckedInt<int64_t> CalculateConvTransposeOutputSize(
    uint32_t aInputSize, uint32_t aFilterSize, uint32_t aBeginningPadding,
    uint32_t aEndingPadding, uint32_t aStride, uint32_t aDilation) {
  CheckedInt<int64_t> effectiveFilterSize =
      (CheckedInt<int64_t>(aFilterSize) - 1) * aDilation + 1;
  return (CheckedInt<int64_t>(aInputSize) - 1) * aStride + effectiveFilterSize -
         aBeginningPadding - aEndingPadding;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-convtranspose2d
bool ValidateConvTranspose2d(const dom::MLOperand& aInput,
                             const dom::MLOperand& aFilter,
                             const dom::MLConvTranspose2dOptions& aOptions,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv) {
  if (aInput.Shape().Length() != 4) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "input must be a 4-D tensor");
    return false;
  }
  if (!IsDataTypeIn(aInput.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "unsupported input data type");
    return false;
  }
  if (aFilter.Shape().Length() != 4) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "filter must be a 4-D tensor");
    return false;
  }
  if (aFilter.DataType() != aInput.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "filter data type must match input");
    return false;
  }
  std::array<uint32_t, 4> padding;
  if (!ResolveListOption(aOptions.mPadding, 0, padding)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "padding must have 4 items");
    return false;
  }
  std::array<uint32_t, 2> strides;
  if (!ResolveListOption(aOptions.mStrides, 1, strides)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "strides must have 2 items");
    return false;
  }
  if (ContainsZero(strides)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "strides must be positive");
    return false;
  }
  std::array<uint32_t, 2> dilations;
  if (!ResolveListOption(aOptions.mDilations, 1, dilations)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "dilations must have 2 items");
    return false;
  }
  if (ContainsZero(dilations)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "dilations must be positive");
    return false;
  }
  std::array<uint32_t, 2> outputPadding;
  if (!ResolveListOption(aOptions.mOutputPadding, 0, outputPadding)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "outputPadding must have 2 items");
    return false;
  }
  if (aOptions.mOutputSizes.WasPassed()) {
    if (aOptions.mOutputSizes.Value().Length() != 2) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                       "outputSizes must have 2 items");
      return false;
    }
  } else if (outputPadding[0] >= strides[0] || outputPadding[1] >= strides[1]) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "outputPadding must be smaller than strides");
    return false;
  }
  uint32_t groups = aOptions.mGroups;
  if (!groups) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "groups must be positive");
    return false;
  }

  const nsTArray<uint32_t>& in = aInput.Shape();
  uint32_t batches = in[0];
  uint32_t inputChannels = 0, inputHeight = 0, inputWidth = 0;
  switch (aOptions.mInputLayout) {
    case MLInputOperandLayout::Nchw:
      inputChannels = in[1];
      inputHeight = in[2];
      inputWidth = in[3];
      break;
    case MLInputOperandLayout::Nhwc:
      inputHeight = in[1];
      inputWidth = in[2];
      inputChannels = in[3];
      break;
  }

  const nsTArray<uint32_t>& f = aFilter.Shape();
  uint32_t filterInputChannels = 0, filterOutputChannels = 0, filterHeight = 0,
           filterWidth = 0;
  switch (aOptions.mFilterLayout) {
    case dom::MLConvTranspose2dFilterOperandLayout::Iohw:
      filterInputChannels = f[0];
      filterOutputChannels = f[1];
      filterHeight = f[2];
      filterWidth = f[3];
      break;
    case dom::MLConvTranspose2dFilterOperandLayout::Hwoi:
      filterHeight = f[0];
      filterWidth = f[1];
      filterOutputChannels = f[2];
      filterInputChannels = f[3];
      break;
    case dom::MLConvTranspose2dFilterOperandLayout::Ohwi:
      filterOutputChannels = f[0];
      filterHeight = f[1];
      filterWidth = f[2];
      filterInputChannels = f[3];
      break;
  }

  if (inputChannels != filterInputChannels) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "filter input channels must match input channels");
    return false;
  }
  CheckedInt<int64_t> checkedOutputChannels =
      CheckedInt<int64_t>(filterOutputChannels) * groups;
  if (!checkedOutputChannels.isValid() ||
      !IsValidDimension(checkedOutputChannels.value())) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "output channels is too large");
    return false;
  }
  uint32_t outputChannels = uint32_t(checkedOutputChannels.value());
  if (aOptions.mBias.WasPassed()) {
    const dom::MLOperand& bias = aOptions.mBias.Value();
    if (bias.Shape().Length() != 1 || bias.Shape()[0] != outputChannels) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                       "bias shape must be [outputChannels]");
      return false;
    }
    if (bias.DataType() != aInput.DataType()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                       "bias data type must match input");
      return false;
    }
  }

  CheckedInt<int64_t> calculatedHeight =
      CalculateConvTransposeOutputSize(inputHeight, filterHeight, padding[0],
                                       padding[1], strides[0], dilations[0]);
  CheckedInt<int64_t> calculatedWidth =
      CalculateConvTransposeOutputSize(inputWidth, filterWidth, padding[2],
                                       padding[3], strides[1], dilations[1]);
  // Overflow only happens for huge positive values, which are never valid
  // output dimensions and never satisfy the outputSizes range checks.
  if (!calculatedHeight.isValid() || !calculatedWidth.isValid()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "invalid output size");
    return false;
  }
  int64_t outputHeight, outputWidth;
  if (aOptions.mOutputSizes.WasPassed()) {
    outputHeight = aOptions.mOutputSizes.Value()[0];
    outputWidth = aOptions.mOutputSizes.Value()[1];
    if (outputHeight < calculatedHeight.value() ||
        outputHeight >= calculatedHeight.value() + strides[0]) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                       "invalid outputSizes height");
      return false;
    }
    if (outputWidth < calculatedWidth.value() ||
        outputWidth >= calculatedWidth.value() + strides[1]) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                       "invalid outputSizes width");
      return false;
    }
  } else {
    outputHeight = calculatedHeight.value() + outputPadding[0];
    outputWidth = calculatedWidth.value() + outputPadding[1];
  }
  if (!IsValidDimension(outputHeight) || !IsValidDimension(outputWidth)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "convTranspose2d",
                     "invalid output size");
    return false;
  }

  nsTArray<uint32_t> outputShape;
  switch (aOptions.mInputLayout) {
    case MLInputOperandLayout::Nchw:
      outputShape =
          nsTArray<uint32_t>{batches, outputChannels, uint32_t(outputHeight),
                             uint32_t(outputWidth)};
      break;
    case MLInputOperandLayout::Nhwc:
      outputShape = nsTArray<uint32_t>{batches, uint32_t(outputHeight),
                                       uint32_t(outputWidth), outputChannels};
      break;
  }
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "convTranspose2d", aOutputs,
                                 aRv);
}

// https://webmachinelearning.github.io/webnn/#mlgraphbuilder-pooling-op
static bool ValidatePool2dCommon(const char* aOp, const dom::MLOperand& aInput,
                                 const dom::MLPool2dOptions& aOptions,
                                 bool aFloatOnly,
                                 nsTArray<OperandDescriptor>& aOutputs,
                                 ErrorResult& aRv) {
  if (aFloatOnly && !IsDataTypeIn(aInput.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "unsupported input data type");
    return false;
  }
  if (aInput.Shape().Length() != 4) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "input must be a 4-D tensor");
    return false;
  }
  const nsTArray<uint32_t>& in = aInput.Shape();
  uint32_t batches = in[0];
  uint32_t channels = 0, inputHeight = 0, inputWidth = 0;
  switch (aOptions.mLayout) {
    case MLInputOperandLayout::Nchw:
      channels = in[1];
      inputHeight = in[2];
      inputWidth = in[3];
      break;
    case MLInputOperandLayout::Nhwc:
      inputHeight = in[1];
      inputWidth = in[2];
      channels = in[3];
      break;
  }

  std::array<uint32_t, 2> windowDimensions = {inputHeight, inputWidth};
  if (aOptions.mWindowDimensions.WasPassed() &&
      !ResolveListOption(aOptions.mWindowDimensions, 0, windowDimensions)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "windowDimensions must have 2 items");
    return false;
  }
  if (ContainsZero(windowDimensions)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "windowDimensions must be positive");
    return false;
  }
  // Per spec step 10, padding is reset to zeros when outputSizes is given.
  std::array<uint32_t, 4> padding = {0, 0, 0, 0};
  if (!aOptions.mOutputSizes.WasPassed() &&
      !ResolveListOption(aOptions.mPadding, 0, padding)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "padding must have 4 items");
    return false;
  }
  std::array<uint32_t, 2> strides;
  if (!ResolveListOption(aOptions.mStrides, 1, strides)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "strides must have 2 items");
    return false;
  }
  if (ContainsZero(strides)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "strides must be positive");
    return false;
  }
  if (aOptions.mOutputSizes.WasPassed()) {
    const dom::Sequence<uint32_t>& outputSizes = aOptions.mOutputSizes.Value();
    if (outputSizes.Length() != 2) {
      ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                       "outputSizes must have 2 items");
      return false;
    }
    // Literal reading of the spec, which looks garbled.
    if (outputSizes[0] >= strides[0] || outputSizes[1] >= strides[1]) {
      ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                       "outputSizes must be smaller than strides");
      return false;
    }
  }
  std::array<uint32_t, 2> dilations;
  if (!ResolveListOption(aOptions.mDilations, 1, dilations)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "dilations must have 2 items");
    return false;
  }
  if (ContainsZero(dilations)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "dilations must be positive");
    return false;
  }

  Maybe<std::array<ConvOutputSize, 2>> sizes = CalculateConv2dOutputSizes(
      inputHeight, inputWidth, windowDimensions[0], windowDimensions[1],
      padding, strides, dilations);
  if (!sizes) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "invalid output size");
    return false;
  }
  const ConvOutputSize& height = (*sizes)[0];
  const ConvOutputSize& width = (*sizes)[1];
  int64_t outputHeight, outputWidth;
  if (aOptions.mOutputSizes.WasPassed()) {
    outputHeight = aOptions.mOutputSizes.Value()[0];
    outputWidth = aOptions.mOutputSizes.Value()[1];
    bool matchesFloor =
        outputHeight == height.mFloor && outputWidth == width.mFloor;
    bool matchesCeil =
        outputHeight == height.mCeil && outputWidth == width.mCeil;
    if (!matchesFloor && !matchesCeil) {
      ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                       "outputSizes do not match the input");
      return false;
    }
  } else {
    switch (aOptions.mOutputShapeRounding) {
      case dom::MLRoundingType::Floor:
        outputHeight = height.mFloor;
        outputWidth = width.mFloor;
        break;
      case dom::MLRoundingType::Ceil:
        outputHeight = height.mCeil;
        outputWidth = width.mCeil;
        break;
    }
  }
  if (!IsValidDimension(outputHeight) || !IsValidDimension(outputWidth)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "invalid output size");
    return false;
  }

  nsTArray<uint32_t> outputShape;
  switch (aOptions.mLayout) {
    case MLInputOperandLayout::Nchw:
      outputShape = nsTArray<uint32_t>{
          batches, channels, uint32_t(outputHeight), uint32_t(outputWidth)};
      break;
    case MLInputOperandLayout::Nhwc:
      outputShape = nsTArray<uint32_t>{batches, uint32_t(outputHeight),
                                       uint32_t(outputWidth), channels};
      break;
  }
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, aOp, aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-pool2d-average
bool ValidateAveragePool2d(const dom::MLOperand& aInput,
                           const dom::MLPool2dOptions& aOptions,
                           nsTArray<OperandDescriptor>& aOutputs,
                           ErrorResult& aRv) {
  return ValidatePool2dCommon("averagePool2d", aInput, aOptions,
                              /* aFloatOnly */ true, aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-pool2d-l2
bool ValidateL2Pool2d(const dom::MLOperand& aInput,
                      const dom::MLPool2dOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  return ValidatePool2dCommon("l2Pool2d", aInput, aOptions,
                              /* aFloatOnly */ true, aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-pool2d-max
bool ValidateMaxPool2d(const dom::MLOperand& aInput,
                       const dom::MLPool2dOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  return ValidatePool2dCommon("maxPool2d", aInput, aOptions,
                              /* aFloatOnly */ false, aOutputs, aRv);
}

}  // namespace mozilla::webnn
