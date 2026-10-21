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

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-gather
bool ValidateGather(const dom::MLOperand& aInput,
                    const dom::MLOperand& aIndices,
                    const dom::MLGatherOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!IsDataTypeIn(aIndices.DataType(), kIndicesTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gather",
                     "invalid indices data type");
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  uint32_t axis = aOptions.mAxis;
  if (axis >= inputShape.Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gather", "axis out of range");
    return false;
  }
  nsTArray<uint32_t> outputShape;
  outputShape.AppendElements(Span(inputShape).To(axis));
  outputShape.AppendElements(aIndices.Shape());
  outputShape.AppendElements(Span(inputShape).From(axis + 1));
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "gather", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-gatherelements
bool ValidateGatherElements(const dom::MLOperand& aInput,
                            const dom::MLOperand& aIndices,
                            const dom::MLGatherOptions& aOptions,
                            nsTArray<OperandDescriptor>& aOutputs,
                            ErrorResult& aRv) {
  if (!IsDataTypeIn(aIndices.DataType(), kIndicesTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gatherElements",
                     "invalid indices data type");
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  const nsTArray<uint32_t>& indicesShape = aIndices.Shape();
  if (inputShape.IsEmpty() || indicesShape.Length() != inputShape.Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gatherElements",
                     "invalid input or indices rank");
    return false;
  }
  uint32_t axis = aOptions.mAxis;
  if (axis >= inputShape.Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gatherElements",
                     "axis out of range");
    return false;
  }
  for (size_t i = 0; i < inputShape.Length(); i++) {
    if (i != axis && indicesShape[i] != inputShape[i]) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "gatherElements",
                       "invalid indices shape");
      return false;
    }
  }
  // The method steps copy input's descriptor; the spec examples and WPT use
  // indices' shape.
  aOutputs.AppendElement(
      OperandDescriptor{aInput.DataType(), indicesShape.Clone()});
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-gathernd
bool ValidateGatherND(const dom::MLOperand& aInput,
                      const dom::MLOperand& aIndices,
                      const dom::MLOperatorOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!IsDataTypeIn(aIndices.DataType(), kIndicesTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gatherND",
                     "invalid indices data type");
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  const nsTArray<uint32_t>& indicesShape = aIndices.Shape();
  if (inputShape.IsEmpty() || indicesShape.IsEmpty()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gatherND",
                     "input and indices must have a rank of at least 1");
    return false;
  }
  size_t indexableSize = indicesShape.Length() - 1;
  uint32_t coordinateSize = indicesShape[indexableSize];
  if (coordinateSize > inputShape.Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gatherND",
                     "coordinate size exceeds input rank");
    return false;
  }
  nsTArray<uint32_t> outputShape;
  outputShape.AppendElements(Span(indicesShape).To(indexableSize));
  outputShape.AppendElements(Span(inputShape).From(coordinateSize));
  return CreateOperandDescriptor(aInput.DataType(), std::move(outputShape),
                                 aOptions.mLabel, "gatherND", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-scatterelements
bool ValidateScatterElements(const dom::MLOperand& aInput,
                             const dom::MLOperand& aIndices,
                             const dom::MLOperand& aUpdates,
                             const dom::MLScatterOptions& aOptions,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv) {
  if (!IsDataTypeIn(aIndices.DataType(), kIndicesTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterElements",
                     "invalid indices data type");
    return false;
  }
  if (aUpdates.DataType() != aInput.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterElements",
                     "updates and input must have the same data type");
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  const nsTArray<uint32_t>& indicesShape = aIndices.Shape();
  size_t rank = inputShape.Length();
  if (!rank || indicesShape.Length() != rank ||
      aUpdates.Shape().Length() != rank) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterElements",
                     "invalid operand rank");
    return false;
  }
  uint32_t axis = aOptions.mAxis;
  if (axis >= rank) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterElements",
                     "axis out of range");
    return false;
  }
  for (size_t i = 0; i < rank; i++) {
    if (i != axis && indicesShape[i] != inputShape[i]) {
      ThrowOpTypeError(aRv, aOptions.mLabel, "scatterElements",
                       "invalid indices shape");
      return false;
    }
  }
  if (aUpdates.Shape() != indicesShape) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterElements",
                     "updates must have the same shape as indices");
    return false;
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-scatternd
bool ValidateScatterND(const dom::MLOperand& aInput,
                       const dom::MLOperand& aIndices,
                       const dom::MLOperand& aUpdates,
                       const dom::MLOperatorOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  if (!IsDataTypeIn(aIndices.DataType(), kIndicesTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterND",
                     "invalid indices data type");
    return false;
  }
  if (aUpdates.DataType() != aInput.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterND",
                     "updates and input must have the same data type");
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  const nsTArray<uint32_t>& indicesShape = aIndices.Shape();
  if (inputShape.IsEmpty() || indicesShape.IsEmpty()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterND",
                     "input and indices must have a rank of at least 1");
    return false;
  }
  size_t indexableSize = indicesShape.Length() - 1;
  uint32_t coordinateSize = indicesShape[indexableSize];
  if (coordinateSize > inputShape.Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterND",
                     "coordinate size exceeds input rank");
    return false;
  }
  nsTArray<uint32_t> expectedUpdatesShape;
  expectedUpdatesShape.AppendElements(Span(indicesShape).To(indexableSize));
  expectedUpdatesShape.AppendElements(Span(inputShape).From(coordinateSize));
  if (aUpdates.Shape() != expectedUpdatesShape) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "scatterND",
                     "invalid updates shape");
    return false;
  }
  return CreateOperandDescriptor(aInput.DataType(), inputShape.Clone(),
                                 aOptions.mLabel, "scatterND", aOutputs, aRv);
}

}  // namespace mozilla::webnn
