/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNUtils.h"
#include "mozilla/webnn/WebNNValidation.h"
#include "nsPrintfCString.h"

namespace mozilla::webnn {

// Checks an optional operand whose allowed data type is "same as input" and
// whose shape must equal aShape.
static bool CheckRecurrentOption(
    const dom::Optional<OwningNonNull<dom::MLOperand>>& aOperand,
    dom::MLOperandDataType aInputType, std::initializer_list<uint64_t> aShape,
    const char* aOp, const char* aName, const nsAString& aLabel,
    ErrorResult& aRv) {
  if (!aOperand.WasPassed()) {
    return true;
  }
  const dom::MLOperand& operand = aOperand.Value();
  if (operand.DataType() != aInputType) {
    ThrowOpTypeError(aRv, aLabel, aOp,
                     nsPrintfCString("invalid %s data type", aName).get());
    return false;
  }
  if (!ShapeEquals(operand.Shape(), aShape)) {
    ThrowOpTypeError(aRv, aLabel, aOp,
                     nsPrintfCString("invalid %s shape", aName).get());
    return false;
  }
  return true;
}

// Steps shared by all four operators: input data type is "float32" or
// "float16", the other listed operands have the same data type as input, and
// each operand has its allowed rank.
static bool CheckRecurrentOperands(
    std::initializer_list<const dom::MLOperand*> aOperands,
    std::initializer_list<size_t> aRanks, const char* aOp,
    const nsAString& aLabel, ErrorResult& aRv) {
  dom::MLOperandDataType inputType = (*aOperands.begin())->DataType();
  bool typesValid = IsDataTypeIn(inputType, kFloatTypes);
  for (const dom::MLOperand* operand : aOperands) {
    typesValid = typesValid && operand->DataType() == inputType;
  }
  if (!typesValid) {
    ThrowOpTypeError(aRv, aLabel, aOp, "invalid data type");
    return false;
  }
  auto rank = aRanks.begin();
  for (const dom::MLOperand* operand : aOperands) {
    if (operand->Shape().Length() != *rank++) {
      ThrowOpTypeError(aRv, aLabel, aOp, "invalid operand rank");
      return false;
    }
  }
  return true;
}

static bool CheckActivations(
    const dom::Optional<dom::Sequence<dom::MLRecurrentNetworkActivation>>&
        aActivations,
    size_t aCount, const char* aOp, const nsAString& aLabel, ErrorResult& aRv) {
  if (aActivations.WasPassed() && aActivations.Value().Length() != aCount) {
    ThrowOpTypeError(
        aRv, aLabel, aOp,
        nsPrintfCString("activations must have %zu items", aCount).get());
    return false;
  }
  return true;
}

static bool CheckHiddenSize(uint64_t aHiddenSize, uint64_t aFactor,
                            const char* aOp, const nsAString& aLabel,
                            ErrorResult& aRv) {
  if (!IsValidDimension(aHiddenSize * aFactor)) {
    ThrowOpTypeError(aRv, aLabel, aOp, "hiddenSize is too large");
    return false;
  }
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-gru
bool ValidateGru(const dom::MLOperand& aInput, const dom::MLOperand& aWeight,
                 const dom::MLOperand& aRecurrentWeight, uint32_t aSteps,
                 uint32_t aHiddenSize, const dom::MLGruOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!CheckRecurrentOperands({&aInput, &aWeight, &aRecurrentWeight}, {3, 3, 3},
                              "gru", aOptions.mLabel, aRv)) {
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  if (inputShape[0] != aSteps) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gru",
                     "input's shape[0] must equal steps");
    return false;
  }
  uint64_t batchSize = inputShape[1];
  uint64_t inputSize = inputShape[2];
  uint64_t hiddenSize = aHiddenSize;
  uint64_t numDirections =
      aOptions.mDirection == dom::MLRecurrentNetworkDirection::Both ? 2 : 1;
  if (!ShapeEquals(aWeight.Shape(),
                   {numDirections, 3 * hiddenSize, inputSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gru", "invalid weight shape");
    return false;
  }
  if (!ShapeEquals(aRecurrentWeight.Shape(),
                   {numDirections, 3 * hiddenSize, hiddenSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gru",
                     "invalid recurrentWeight shape");
    return false;
  }
  dom::MLOperandDataType type = aInput.DataType();
  if (!CheckHiddenSize(hiddenSize, 6, "gru", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mBias, type,
                            {numDirections, 3 * hiddenSize}, "gru", "bias",
                            aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mRecurrentBias, type,
                            {numDirections, 3 * hiddenSize}, "gru",
                            "recurrentBias", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mInitialHiddenState, type,
                            {numDirections, batchSize, hiddenSize}, "gru",
                            "initialHiddenState", aOptions.mLabel, aRv) ||
      !CheckActivations(aOptions.mActivations, 2, "gru", aOptions.mLabel,
                        aRv)) {
    return false;
  }
  // desc1 is created (and so validated) even if returnSequence is false.
  nsTArray<OperandDescriptor> descs;
  if (!CreateOperandDescriptor(
          type, {uint32_t(numDirections), uint32_t(batchSize), aHiddenSize},
          aOptions.mLabel, "gru", descs, aRv) ||
      !CreateOperandDescriptor(
          type,
          {aSteps, uint32_t(numDirections), uint32_t(batchSize), aHiddenSize},
          aOptions.mLabel, "gru", descs, aRv)) {
    return false;
  }
  if (!aOptions.mReturnSequence) {
    descs.RemoveLastElement();
  }
  aOutputs.AppendElements(std::move(descs));
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-grucell
bool ValidateGruCell(const dom::MLOperand& aInput,
                     const dom::MLOperand& aWeight,
                     const dom::MLOperand& aRecurrentWeight,
                     const dom::MLOperand& aHiddenState, uint32_t aHiddenSize,
                     const dom::MLGruCellOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!CheckRecurrentOperands(
          {&aInput, &aWeight, &aRecurrentWeight, &aHiddenState}, {2, 2, 2, 2},
          "gruCell", aOptions.mLabel, aRv)) {
    return false;
  }
  uint64_t batchSize = aInput.Shape()[0];
  uint64_t inputSize = aInput.Shape()[1];
  uint64_t hiddenSize = aHiddenSize;
  if (!ShapeEquals(aWeight.Shape(), {3 * hiddenSize, inputSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gruCell", "invalid weight shape");
    return false;
  }
  if (!ShapeEquals(aRecurrentWeight.Shape(), {3 * hiddenSize, hiddenSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gruCell",
                     "invalid recurrentWeight shape");
    return false;
  }
  if (!ShapeEquals(aHiddenState.Shape(), {batchSize, hiddenSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "gruCell",
                     "invalid hiddenState shape");
    return false;
  }
  dom::MLOperandDataType type = aInput.DataType();
  if (!CheckHiddenSize(hiddenSize, 6, "gruCell", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mBias, type, {3 * hiddenSize}, "gruCell",
                            "bias", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mRecurrentBias, type, {3 * hiddenSize},
                            "gruCell", "recurrentBias", aOptions.mLabel, aRv) ||
      !CheckActivations(aOptions.mActivations, 2, "gruCell", aOptions.mLabel,
                        aRv)) {
    return false;
  }
  return CreateOperandDescriptor(type, {uint32_t(batchSize), aHiddenSize},
                                 aOptions.mLabel, "gruCell", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-lstm
bool ValidateLstm(const dom::MLOperand& aInput, const dom::MLOperand& aWeight,
                  const dom::MLOperand& aRecurrentWeight, uint32_t aSteps,
                  uint32_t aHiddenSize, const dom::MLLstmOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  uint64_t numDirections =
      aOptions.mDirection == dom::MLRecurrentNetworkDirection::Both ? 2 : 1;
  if (!CheckRecurrentOperands({&aInput, &aWeight, &aRecurrentWeight}, {3, 3, 3},
                              "lstm", aOptions.mLabel, aRv)) {
    return false;
  }
  if (!aSteps) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstm", "steps must not be 0");
    return false;
  }
  const nsTArray<uint32_t>& inputShape = aInput.Shape();
  if (inputShape[0] != aSteps) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstm",
                     "input's shape[0] must equal steps");
    return false;
  }
  uint64_t batchSize = inputShape[1];
  uint64_t inputSize = inputShape[2];
  uint64_t hiddenSize = aHiddenSize;
  if (!ShapeEquals(aWeight.Shape(),
                   {numDirections, 4 * hiddenSize, inputSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstm", "invalid weight shape");
    return false;
  }
  if (!ShapeEquals(aRecurrentWeight.Shape(),
                   {numDirections, 4 * hiddenSize, hiddenSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstm",
                     "invalid recurrentWeight shape");
    return false;
  }
  dom::MLOperandDataType type = aInput.DataType();
  if (!CheckHiddenSize(hiddenSize, 8, "lstm", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mBias, type,
                            {numDirections, 4 * hiddenSize}, "lstm", "bias",
                            aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mRecurrentBias, type,
                            {numDirections, 4 * hiddenSize}, "lstm",
                            "recurrentBias", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mPeepholeWeight, type,
                            {numDirections, 3 * hiddenSize}, "lstm",
                            "peepholeWeight", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mInitialHiddenState, type,
                            {numDirections, batchSize, hiddenSize}, "lstm",
                            "initialHiddenState", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mInitialCellState, type,
                            {numDirections, batchSize, hiddenSize}, "lstm",
                            "initialCellState", aOptions.mLabel, aRv) ||
      !CheckActivations(aOptions.mActivations, 3, "lstm", aOptions.mLabel,
                        aRv)) {
    return false;
  }
  // desc2 is created (and so validated) even if returnSequence is false.
  nsTArray<OperandDescriptor> descs;
  if (!CreateOperandDescriptor(
          type, {uint32_t(numDirections), uint32_t(batchSize), aHiddenSize},
          aOptions.mLabel, "lstm", descs, aRv) ||
      !CreateOperandDescriptor(
          type,
          {aSteps, uint32_t(numDirections), uint32_t(batchSize), aHiddenSize},
          aOptions.mLabel, "lstm", descs, aRv)) {
    return false;
  }
  aOutputs.AppendElement(OperandDescriptor{type, descs[0].mShape.Clone()});
  aOutputs.AppendElement(std::move(descs[0]));
  if (aOptions.mReturnSequence) {
    aOutputs.AppendElement(std::move(descs[1]));
  }
  return true;
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-lstmcell
bool ValidateLstmCell(const dom::MLOperand& aInput,
                      const dom::MLOperand& aWeight,
                      const dom::MLOperand& aRecurrentWeight,
                      const dom::MLOperand& aHiddenState,
                      const dom::MLOperand& aCellState, uint32_t aHiddenSize,
                      const dom::MLLstmCellOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!CheckRecurrentOperands(
          {&aInput, &aWeight, &aRecurrentWeight, &aHiddenState, &aCellState},
          {2, 2, 2, 2, 2}, "lstmCell", aOptions.mLabel, aRv)) {
    return false;
  }
  uint64_t batchSize = aInput.Shape()[0];
  uint64_t inputSize = aInput.Shape()[1];
  uint64_t hiddenSize = aHiddenSize;
  if (!ShapeEquals(aWeight.Shape(), {4 * hiddenSize, inputSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstmCell", "invalid weight shape");
    return false;
  }
  if (!ShapeEquals(aRecurrentWeight.Shape(), {4 * hiddenSize, hiddenSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstmCell",
                     "invalid recurrentWeight shape");
    return false;
  }
  if (!ShapeEquals(aHiddenState.Shape(), {batchSize, hiddenSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstmCell",
                     "invalid hiddenState shape");
    return false;
  }
  if (!ShapeEquals(aCellState.Shape(), {batchSize, hiddenSize})) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "lstmCell",
                     "invalid cellState shape");
    return false;
  }
  dom::MLOperandDataType type = aInput.DataType();
  if (!CheckHiddenSize(hiddenSize, 8, "lstmCell", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mBias, type, {4 * hiddenSize}, "lstmCell",
                            "bias", aOptions.mLabel, aRv) ||
      !CheckRecurrentOption(aOptions.mRecurrentBias, type, {4 * hiddenSize},
                            "lstmCell", "recurrentBias", aOptions.mLabel,
                            aRv) ||
      !CheckRecurrentOption(aOptions.mPeepholeWeight, type, {3 * hiddenSize},
                            "lstmCell", "peepholeWeight", aOptions.mLabel,
                            aRv) ||
      !CheckActivations(aOptions.mActivations, 3, "lstmCell", aOptions.mLabel,
                        aRv)) {
    return false;
  }
  // The spec builds desc directly, without "create an MLOperandDescriptor";
  // its shape equals hiddenState's, which is already valid.
  nsTArray<uint32_t> shape{uint32_t(batchSize), aHiddenSize};
  aOutputs.AppendElement(OperandDescriptor{type, shape.Clone()});
  aOutputs.AppendElement(OperandDescriptor{type, std::move(shape)});
  return true;
}

}  // namespace mozilla::webnn
