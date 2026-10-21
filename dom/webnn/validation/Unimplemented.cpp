/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Validators that are not written yet. Remove a stub when implementing its
// operator in another file. `./mach python dom/webnn/codegen/webnn_codegen.py
// --stubs <op>...` prints stubs, e.g. after adding operators to WebNN.webidl.

#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

bool ValidateConcat(const dom::Sequence<OwningNonNull<dom::MLOperand>>& aInputs,
                    uint32_t aAxis, const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("concat is not implemented yet");
  return false;
}

bool ValidateExpand(const dom::MLOperand& aInput,
                    const dom::Sequence<uint32_t>& aNewShape,
                    const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("expand is not implemented yet");
  return false;
}

bool ValidateGather(const dom::MLOperand& aInput,
                    const dom::MLOperand& aIndices,
                    const dom::MLGatherOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("gather is not implemented yet");
  return false;
}

bool ValidateGatherElements(const dom::MLOperand& aInput,
                            const dom::MLOperand& aIndices,
                            const dom::MLGatherOptions& aOptions,
                            nsTArray<OperandDescriptor>& aOutputs,
                            ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("gatherElements is not implemented yet");
  return false;
}

bool ValidateGatherND(const dom::MLOperand& aInput,
                      const dom::MLOperand& aIndices,
                      const dom::MLOperatorOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("gatherND is not implemented yet");
  return false;
}

bool ValidateGru(const dom::MLOperand& aInput, const dom::MLOperand& aWeight,
                 const dom::MLOperand& aRecurrentWeight, uint32_t aSteps,
                 uint32_t aHiddenSize, const dom::MLGruOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("gru is not implemented yet");
  return false;
}

bool ValidateGruCell(const dom::MLOperand& aInput,
                     const dom::MLOperand& aWeight,
                     const dom::MLOperand& aRecurrentWeight,
                     const dom::MLOperand& aHiddenState, uint32_t aHiddenSize,
                     const dom::MLGruCellOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("gruCell is not implemented yet");
  return false;
}

bool ValidateLstm(const dom::MLOperand& aInput, const dom::MLOperand& aWeight,
                  const dom::MLOperand& aRecurrentWeight, uint32_t aSteps,
                  uint32_t aHiddenSize, const dom::MLLstmOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("lstm is not implemented yet");
  return false;
}

bool ValidateLstmCell(const dom::MLOperand& aInput,
                      const dom::MLOperand& aWeight,
                      const dom::MLOperand& aRecurrentWeight,
                      const dom::MLOperand& aHiddenState,
                      const dom::MLOperand& aCellState, uint32_t aHiddenSize,
                      const dom::MLLstmCellOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("lstmCell is not implemented yet");
  return false;
}

bool ValidatePad(const dom::MLOperand& aInput,
                 const dom::Sequence<uint32_t>& aBeginningPadding,
                 const dom::Sequence<uint32_t>& aEndingPadding,
                 const dom::MLPadOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("pad is not implemented yet");
  return false;
}

bool ValidateReshape(const dom::MLOperand& aInput,
                     const dom::Sequence<uint32_t>& aNewShape,
                     const dom::MLOperatorOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reshape is not implemented yet");
  return false;
}

bool ValidateReverse(const dom::MLOperand& aInput,
                     const dom::MLReverseOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reverse is not implemented yet");
  return false;
}

bool ValidateScatterElements(const dom::MLOperand& aInput,
                             const dom::MLOperand& aIndices,
                             const dom::MLOperand& aUpdates,
                             const dom::MLScatterOptions& aOptions,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("scatterElements is not implemented yet");
  return false;
}

bool ValidateScatterND(const dom::MLOperand& aInput,
                       const dom::MLOperand& aIndices,
                       const dom::MLOperand& aUpdates,
                       const dom::MLOperatorOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("scatterND is not implemented yet");
  return false;
}

bool ValidateSlice(const dom::MLOperand& aInput,
                   const dom::Sequence<uint32_t>& aStarts,
                   const dom::Sequence<uint32_t>& aSizes,
                   const dom::MLSliceOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("slice is not implemented yet");
  return false;
}

bool ValidateSplit(
    const dom::MLOperand& aInput,
    const dom::RangeEnforcedUnsignedLongOrRangeEnforcedUnsignedLongSequence&
        aSplits,
    const dom::MLSplitOptions& aOptions, nsTArray<OperandDescriptor>& aOutputs,
    ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("split is not implemented yet");
  return false;
}

bool ValidateTile(const dom::MLOperand& aInput,
                  const dom::Sequence<uint32_t>& aRepetitions,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("tile is not implemented yet");
  return false;
}

bool ValidateTranspose(const dom::MLOperand& aInput,
                       const dom::MLTransposeOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("transpose is not implemented yet");
  return false;
}

bool ValidateTriangular(const dom::MLOperand& aInput,
                        const dom::MLTriangularOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("triangular is not implemented yet");
  return false;
}

}  // namespace mozilla::webnn
