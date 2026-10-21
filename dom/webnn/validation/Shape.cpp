/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNUtils.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-reshape-method
bool ValidateReshape(const dom::MLOperand& aInput,
                     const dom::Sequence<uint32_t>& aNewShape,
                     const dom::MLOperatorOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  CheckedInt<uint64_t> newCount = 1;
  for (uint32_t dim : aNewShape) {
    if (!IsValidDimension(dim)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "reshape",
                       "invalid dimension in newShape");
      return false;
    }
    newCount *= dim;
  }
  uint64_t inputCount = 1;
  for (uint32_t dim : aInput.Shape()) {
    inputCount *= dim;
  }
  if (!newCount.isValid() || newCount.value() != inputCount) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "reshape", "element count mismatch");
    return false;
  }
  return CreateOperandDescriptor(aInput.DataType(), ToShape(aNewShape),
                                 aOptions.mLabel, "reshape", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-transpose
bool ValidateTranspose(const dom::MLOperand& aInput,
                       const dom::MLTransposeOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  size_t rank = inputShape.Length();
  nsTArray<uint32_t> outputShape(rank);
  if (!aOptions.mPermutation.WasPassed()) {
    for (size_t i = 0; i < rank; i++) {
      outputShape.AppendElement(inputShape[rank - 1 - i]);
    }
  } else {
    const dom::Sequence<uint32_t>& permutation = aOptions.mPermutation.Value();
    if (permutation.Length() != rank) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "transpose",
                       "permutation size must equal input rank");
      return false;
    }
    nsTArray<bool> seen;
    seen.InsertElementsAt(0, rank, false);
    for (uint32_t axis : permutation) {
      if (axis >= rank) {
        ThrowOpTypeError(aRv, aOptions.mLabel, "transpose",
                         "permutation value out of range");
        return false;
      }
      if (seen[axis]) {
        ThrowOpTypeError(aRv, aOptions.mLabel, "transpose",
                         "duplicate permutation value");
        return false;
      }
      seen[axis] = true;
      outputShape.AppendElement(inputShape[axis]);
    }
  }
  aOutputs.AppendElement(
      OperandDescriptor{aInput.DataType(), std::move(outputShape)});
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-slice
bool ValidateSlice(const dom::MLOperand& aInput,
                   const dom::Sequence<uint32_t>& aStarts,
                   const dom::Sequence<uint32_t>& aSizes,
                   const dom::MLSliceOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  for (uint32_t size : aSizes) {
    if (!size) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "slice", "sizes must not be 0");
      return false;
    }
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  size_t rank = inputShape.Length();
  if (aStarts.Length() != rank || aSizes.Length() != rank) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "slice",
                     "starts and sizes must have the input rank");
    return false;
  }
  if (aOptions.mStrides.WasPassed() &&
      aOptions.mStrides.Value().Length() != rank) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "slice",
                     "strides must have the input rank");
    return false;
  }
  nsTArray<uint32_t> outputShape(rank);
  for (size_t i = 0; i < rank; i++) {
    uint64_t inputSize = inputShape[i];
    uint64_t sliceSize = aSizes[i];
    uint64_t stride =
        aOptions.mStrides.WasPassed() ? aOptions.mStrides.Value()[i] : 1;
    if (stride < 1) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "slice",
                       "strides must be greater than 0");
      return false;
    }
    if (aStarts[i] > inputSize || aStarts[i] + sliceSize > inputSize) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "slice", "slice is out of bounds");
      return false;
    }
    outputShape.AppendElement(
        uint32_t(sliceSize / stride + (sliceSize % stride ? 1 : 0)));
  }
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "slice", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-concat
bool ValidateConcat(const dom::Sequence<OwningNonNull<dom::MLOperand>>& aInputs,
                    uint32_t aAxis, const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!IsValidTensorCount(aInputs.Length())) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "concat",
                     "invalid number of inputs");
    return false;
  }
  const dom::MLOperand& first = aInputs[0];
  if (aAxis >= first.Shape().Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "concat", "axis out of range");
    return false;
  }
  nsTArray<uint32_t> outputShape = first.Shape().Clone();
  for (size_t index = 1; index < aInputs.Length(); index++) {
    const dom::MLOperand& input = aInputs[index];
    if (input.DataType() != first.DataType()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "concat",
                       "inputs must have the same data type");
      return false;
    }
    if (input.Shape().Length() != first.Shape().Length()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "concat",
                       "inputs must have the same rank");
      return false;
    }
    for (size_t dim = 0; dim < input.Shape().Length(); dim++) {
      if (dim != aAxis) {
        if (input.Shape()[dim] != first.Shape()[dim]) {
          ThrowOpTypeError(aRv, aOptions.mLabel, "concat",
                           "input shapes do not match");
          return false;
        }
        continue;
      }
      uint64_t size = uint64_t(outputShape[aAxis]) + input.Shape()[dim];
      if (!IsValidDimension(size)) {
        ThrowOpTypeError(aRv, aOptions.mLabel, "concat",
                         "output dimension is too large");
        return false;
      }
      outputShape[aAxis] = uint32_t(size);
    }
  }
  return CreateOperandDescriptor(first.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "concat", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-split
bool ValidateSplit(
    const dom::MLOperand& aInput,
    const dom::RangeEnforcedUnsignedLongOrRangeEnforcedUnsignedLongSequence&
        aSplits,
    const dom::MLSplitOptions& aOptions, nsTArray<OperandDescriptor>& aOutputs,
    ErrorResult& aRv) {
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  uint32_t axis = aOptions.mAxis;
  if (axis >= inputShape.Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "split", "axis out of range");
    return false;
  }
  if (aSplits.IsRangeEnforcedUnsignedLong()) {
    uint32_t splits = aSplits.GetAsRangeEnforcedUnsignedLong();
    if (!IsValidTensorCount(splits)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "split",
                       "invalid number of splits");
      return false;
    }
    if (inputShape[axis] % splits) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "split",
                       "splits must evenly divide the axis");
      return false;
    }
    for (uint32_t i = 0; i < splits; i++) {
      nsTArray<uint32_t> shape = inputShape.Clone();
      shape[axis] /= splits;
      aOutputs.AppendElement(
          OperandDescriptor{aInput.DataType(), std::move(shape)});
    }
    return true;
  }
  const dom::Sequence<uint32_t>& splits =
      aSplits.GetAsRangeEnforcedUnsignedLongSequence();
  if (!IsValidTensorCount(splits.Length())) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "split", "invalid number of splits");
    return false;
  }
  uint64_t sum = 0;
  for (uint32_t size : splits) {
    if (!size) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "split",
                       "split sizes must not be 0");
      return false;
    }
    sum += size;
  }
  if (sum != inputShape[axis]) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "split",
                     "split sizes must sum to the axis dimension");
    return false;
  }
  for (uint32_t size : splits) {
    nsTArray<uint32_t> shape = inputShape.Clone();
    shape[axis] = size;
    aOutputs.AppendElement(
        OperandDescriptor{aInput.DataType(), std::move(shape)});
  }
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-pad
bool ValidatePad(const dom::MLOperand& aInput,
                 const dom::Sequence<uint32_t>& aBeginningPadding,
                 const dom::Sequence<uint32_t>& aEndingPadding,
                 const dom::MLPadOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  size_t rank = inputShape.Length();
  if (aBeginningPadding.Length() != rank || aEndingPadding.Length() != rank) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "pad",
                     "paddings must have the input rank");
    return false;
  }
  nsTArray<uint32_t> outputShape(rank);
  for (size_t i = 0; i < rank; i++) {
    if (aOptions.mMode == dom::MLPaddingMode::Reflection &&
        (aBeginningPadding[i] >= inputShape[i] ||
         aEndingPadding[i] >= inputShape[i])) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "pad",
                       "reflection padding must be smaller than the dimension");
      return false;
    }
    uint64_t size =
        uint64_t(inputShape[i]) + aBeginningPadding[i] + aEndingPadding[i];
    if (!IsValidDimension(size)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "pad",
                       "output dimension is too large");
      return false;
    }
    outputShape.AppendElement(uint32_t(size));
  }
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "pad", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-expand
bool ValidateExpand(const dom::MLOperand& aInput,
                    const dom::Sequence<uint32_t>& aNewShape,
                    const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  // Unidirectional broadcasting:
  // https://webmachinelearning.github.io/webnn/#unidirectionally-broadcast-the-shapes
  const nsTArray<uint32_t>& from = aInput.Shape();
  size_t sizeTo = aNewShape.Length();
  if (from.Length() > sizeTo) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "expand",
                     "input is not broadcastable to newShape");
    return false;
  }
  size_t padding = sizeTo - from.Length();
  for (size_t i = 0; i < sizeTo; i++) {
    uint32_t dimFrom = i < padding ? 1 : from[i - padding];
    if (aNewShape[i] != dimFrom && dimFrom != 1) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "expand",
                       "input is not broadcastable to newShape");
      return false;
    }
  }
  return CreateOperandDescriptor(aInput.DataType(), ToShape(aNewShape),
                                 aOptions.mLabel, "expand", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-tile
bool ValidateTile(const dom::MLOperand& aInput,
                  const dom::Sequence<uint32_t>& aRepetitions,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  size_t rank = inputShape.Length();
  if (aRepetitions.Length() != rank) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "tile",
                     "repetitions must have the input rank");
    return false;
  }
  for (uint32_t repetition : aRepetitions) {
    if (!repetition) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "tile",
                       "repetitions must not be 0");
      return false;
    }
  }
  nsTArray<uint32_t> outputShape(rank);
  for (size_t i = 0; i < rank; i++) {
    uint64_t size = uint64_t(inputShape[i]) * aRepetitions[i];
    if (!IsValidDimension(size)) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "tile",
                       "output dimension is too large");
      return false;
    }
    outputShape.AppendElement(uint32_t(size));
  }
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "tile", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-reverse-method
bool ValidateReverse(const dom::MLOperand& aInput,
                     const dom::MLReverseOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (aOptions.mAxes.WasPassed()) {
    size_t rank = aInput.Shape().Length();
    nsTArray<bool> seen;
    seen.InsertElementsAt(0, rank, false);
    for (uint32_t axis : aOptions.mAxes.Value()) {
      if (axis >= rank || seen[axis]) {
        ThrowOpTypeError(aRv, aOptions.mLabel, "reverse",
                         "invalid or duplicate axis");
        return false;
      }
      seen[axis] = true;
    }
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-triangular
bool ValidateTriangular(const dom::MLOperand& aInput,
                        const dom::MLTriangularOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  if (aInput.Shape().Length() < 2) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "triangular",
                     "input must have a rank of at least 2");
    return false;
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

}  // namespace mozilla::webnn
