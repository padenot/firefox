/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <cmath>

#include "ValidationUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

// Computes floor(aSize * aScale) exactly, aScale being positive and finite.
// Returns Nothing() if the result is not a valid dimension.
static Maybe<uint32_t> ResampleScaledSize(uint32_t aSize, float aScale) {
  // aScale == significand * 2^exponent, with a 24-bit integer significand.
  int exponent;
  double fraction = std::frexp(double(aScale), &exponent);
  uint64_t significand = uint64_t(std::ldexp(fraction, 24));
  exponent -= 24;
  uint64_t product = uint64_t(aSize) * significand;
  uint64_t size;
  if (exponent >= 0) {
    if (exponent >= 32 || product > (uint64_t(INT32_MAX) >> exponent)) {
      return Nothing();
    }
    size = product << exponent;
  } else {
    size = -exponent >= 64 ? 0 : product >> -exponent;
  }
  if (!IsValidDimension(int64_t(size))) {
    return Nothing();
  }
  return Some(uint32_t(size));
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-resample2d-method
bool ValidateResample2d(const dom::MLOperand& aInput,
                        const dom::MLResample2dOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  dom::MLOperandDataType type = aInput.DataType();
  constexpr MLOperandDataType kResampleTypes[] = {
      MLOperandDataType::Float32, MLOperandDataType::Float16,
      MLOperandDataType::Uint8, MLOperandDataType::Int8};
  if (!IsDataTypeIn(type, kResampleTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d",
                     "unsupported input data type");
    return false;
  }
  if (aInput.Shape().Length() != 4) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d",
                     "input must be a 4-D tensor");
    return false;
  }
  std::array<float, 2> scales = {1.0f, 1.0f};
  if (aOptions.mScales.WasPassed()) {
    const dom::Sequence<float>& value = aOptions.mScales.Value();
    if (value.Length() != 2) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d",
                       "scales must have 2 items");
      return false;
    }
    for (size_t i = 0; i < 2; i++) {
      if (value[i] <= 0) {
        ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d",
                         "scales must be positive");
        return false;
      }
      scales[i] = value[i];
    }
  }
  if (aOptions.mSizes.WasPassed()) {
    const dom::Sequence<uint32_t>& sizes = aOptions.mSizes.Value();
    if (sizes.Length() != 2 || !sizes[0] || !sizes[1]) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d",
                       "sizes must have 2 positive items");
      return false;
    }
  }
  std::array<uint32_t, 2> axes;
  // The spec does not check the length of axes.
  if (!ResolveListOption(aOptions.mAxes, 0, axes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d",
                     "axes must have 2 items");
    return false;
  }
  if (!aOptions.mAxes.WasPassed()) {
    axes = {2, 3};
  } else if (!AreValidAxes(axes, aInput.Shape().Length())) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d", "invalid axes");
    return false;
  }

  nsTArray<uint32_t> outputShape = aInput.Shape().Clone();
  for (size_t i = 0; i < axes.size(); i++) {
    Maybe<uint32_t> size;
    if (aOptions.mSizes.WasPassed()) {
      uint32_t value = aOptions.mSizes.Value()[i];
      if (IsValidDimension(value)) {
        size = Some(value);
      }
    } else {
      size = ResampleScaledSize(aInput.Shape()[axes[i]], scales[i]);
    }
    if (!size) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "resample2d",
                       "invalid output size");
      return false;
    }
    outputShape[axes[i]] = *size;
  }
  return CreateOperandDescriptor(type, std::move(outputShape), aOptions.mLabel,
                                 "resample2d", aOutputs, aRv);
}

}  // namespace mozilla::webnn
