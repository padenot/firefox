/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Validators that are not written yet. Remove a stub when implementing its
// operator in another file. `./mach python dom/webnn/codegen/webnn_codegen.py
// --stubs <op>...` prints stubs, e.g. after adding operators to WebNN.webidl.

#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

bool ValidateArgMin(const dom::MLOperand& aInput, uint32_t aAxis,
                    const dom::MLArgMinMaxOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("argMin is not implemented yet");
  return false;
}

bool ValidateArgMax(const dom::MLOperand& aInput, uint32_t aAxis,
                    const dom::MLArgMinMaxOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("argMax is not implemented yet");
  return false;
}

bool ValidateBatchNormalization(
    const dom::MLOperand& aInput, const dom::MLOperand& aMean,
    const dom::MLOperand& aVariance,
    const dom::MLBatchNormalizationOptions& aOptions,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("batchNormalization is not implemented yet");
  return false;
}

bool ValidateCast(const dom::MLOperand& aInput,
                  dom::MLOperandDataType aDataType,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("cast is not implemented yet");
  return false;
}

bool ValidateClamp(const dom::MLOperand& aInput,
                   const dom::MLClampOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("clamp is not implemented yet");
  return false;
}

bool ValidateConcat(const dom::Sequence<OwningNonNull<dom::MLOperand>>& aInputs,
                    uint32_t aAxis, const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("concat is not implemented yet");
  return false;
}

bool ValidateConv2d(const dom::MLOperand& aInput, const dom::MLOperand& aFilter,
                    const dom::MLConv2dOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("conv2d is not implemented yet");
  return false;
}

bool ValidateConvTranspose2d(const dom::MLOperand& aInput,
                             const dom::MLOperand& aFilter,
                             const dom::MLConvTranspose2dOptions& aOptions,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("convTranspose2d is not implemented yet");
  return false;
}

bool ValidateCumulativeSum(const dom::MLOperand& aInput, uint32_t aAxis,
                           const dom::MLCumulativeSumOptions& aOptions,
                           nsTArray<OperandDescriptor>& aOutputs,
                           ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("cumulativeSum is not implemented yet");
  return false;
}

bool ValidateSub(const dom::MLOperand& aA, const dom::MLOperand& aB,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("sub is not implemented yet");
  return false;
}

bool ValidateMul(const dom::MLOperand& aA, const dom::MLOperand& aB,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("mul is not implemented yet");
  return false;
}

bool ValidateDiv(const dom::MLOperand& aA, const dom::MLOperand& aB,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("div is not implemented yet");
  return false;
}

bool ValidateMax(const dom::MLOperand& aA, const dom::MLOperand& aB,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("max is not implemented yet");
  return false;
}

bool ValidateMin(const dom::MLOperand& aA, const dom::MLOperand& aB,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("min is not implemented yet");
  return false;
}

bool ValidatePow(const dom::MLOperand& aA, const dom::MLOperand& aB,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("pow is not implemented yet");
  return false;
}

bool ValidateEqual(const dom::MLOperand& aA, const dom::MLOperand& aB,
                   const dom::MLOperatorOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("equal is not implemented yet");
  return false;
}

bool ValidateNotEqual(const dom::MLOperand& aA, const dom::MLOperand& aB,
                      const dom::MLOperatorOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("notEqual is not implemented yet");
  return false;
}

bool ValidateGreater(const dom::MLOperand& aA, const dom::MLOperand& aB,
                     const dom::MLOperatorOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("greater is not implemented yet");
  return false;
}

bool ValidateGreaterOrEqual(const dom::MLOperand& aA, const dom::MLOperand& aB,
                            const dom::MLOperatorOptions& aOptions,
                            nsTArray<OperandDescriptor>& aOutputs,
                            ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("greaterOrEqual is not implemented yet");
  return false;
}

bool ValidateLesser(const dom::MLOperand& aA, const dom::MLOperand& aB,
                    const dom::MLOperatorOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("lesser is not implemented yet");
  return false;
}

bool ValidateLesserOrEqual(const dom::MLOperand& aA, const dom::MLOperand& aB,
                           const dom::MLOperatorOptions& aOptions,
                           nsTArray<OperandDescriptor>& aOutputs,
                           ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("lesserOrEqual is not implemented yet");
  return false;
}

bool ValidateLogicalNot(const dom::MLOperand& aA,
                        const dom::MLOperatorOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("logicalNot is not implemented yet");
  return false;
}

bool ValidateLogicalAnd(const dom::MLOperand& aA, const dom::MLOperand& aB,
                        const dom::MLOperatorOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("logicalAnd is not implemented yet");
  return false;
}

bool ValidateLogicalOr(const dom::MLOperand& aA, const dom::MLOperand& aB,
                       const dom::MLOperatorOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("logicalOr is not implemented yet");
  return false;
}

bool ValidateLogicalXor(const dom::MLOperand& aA, const dom::MLOperand& aB,
                        const dom::MLOperatorOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("logicalXor is not implemented yet");
  return false;
}

bool ValidateIsNaN(const dom::MLOperand& aA,
                   const dom::MLOperatorOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("isNaN is not implemented yet");
  return false;
}

bool ValidateIsInfinite(const dom::MLOperand& aA,
                        const dom::MLOperatorOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("isInfinite is not implemented yet");
  return false;
}

bool ValidateAbs(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("abs is not implemented yet");
  return false;
}

bool ValidateCeil(const dom::MLOperand& aInput,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("ceil is not implemented yet");
  return false;
}

bool ValidateCos(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("cos is not implemented yet");
  return false;
}

bool ValidateErf(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("erf is not implemented yet");
  return false;
}

bool ValidateExp(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("exp is not implemented yet");
  return false;
}

bool ValidateFloor(const dom::MLOperand& aInput,
                   const dom::MLOperatorOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("floor is not implemented yet");
  return false;
}

bool ValidateIdentity(const dom::MLOperand& aInput,
                      const dom::MLOperatorOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("identity is not implemented yet");
  return false;
}

bool ValidateLog(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("log is not implemented yet");
  return false;
}

bool ValidateNeg(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("neg is not implemented yet");
  return false;
}

bool ValidateReciprocal(const dom::MLOperand& aInput,
                        const dom::MLOperatorOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reciprocal is not implemented yet");
  return false;
}

bool ValidateRoundEven(const dom::MLOperand& aInput,
                       const dom::MLOperatorOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("roundEven is not implemented yet");
  return false;
}

bool ValidateSin(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("sin is not implemented yet");
  return false;
}

bool ValidateSign(const dom::MLOperand& aInput,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("sign is not implemented yet");
  return false;
}

bool ValidateSqrt(const dom::MLOperand& aInput,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("sqrt is not implemented yet");
  return false;
}

bool ValidateTan(const dom::MLOperand& aInput,
                 const dom::MLOperatorOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("tan is not implemented yet");
  return false;
}

bool ValidateDequantizeLinear(const dom::MLOperand& aInput,
                              const dom::MLOperand& aScale,
                              const dom::MLOperand& aZeroPoint,
                              const dom::MLOperatorOptions& aOptions,
                              nsTArray<OperandDescriptor>& aOutputs,
                              ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("dequantizeLinear is not implemented yet");
  return false;
}

bool ValidateQuantizeLinear(const dom::MLOperand& aInput,
                            const dom::MLOperand& aScale,
                            const dom::MLOperand& aZeroPoint,
                            const dom::MLOperatorOptions& aOptions,
                            nsTArray<OperandDescriptor>& aOutputs,
                            ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("quantizeLinear is not implemented yet");
  return false;
}

bool ValidateElu(const dom::MLOperand& aInput,
                 const dom::MLEluOptions& aOptions,
                 nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("elu is not implemented yet");
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

bool ValidateGelu(const dom::MLOperand& aInput,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("gelu is not implemented yet");
  return false;
}

bool ValidateGemm(const dom::MLOperand& aA, const dom::MLOperand& aB,
                  const dom::MLGemmOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("gemm is not implemented yet");
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

bool ValidateHardSigmoid(const dom::MLOperand& aInput,
                         const dom::MLHardSigmoidOptions& aOptions,
                         nsTArray<OperandDescriptor>& aOutputs,
                         ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("hardSigmoid is not implemented yet");
  return false;
}

bool ValidateHardSwish(const dom::MLOperand& aInput,
                       const dom::MLOperatorOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("hardSwish is not implemented yet");
  return false;
}

bool ValidateInstanceNormalization(
    const dom::MLOperand& aInput,
    const dom::MLInstanceNormalizationOptions& aOptions,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("instanceNormalization is not implemented yet");
  return false;
}

bool ValidateLayerNormalization(
    const dom::MLOperand& aInput,
    const dom::MLLayerNormalizationOptions& aOptions,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("layerNormalization is not implemented yet");
  return false;
}

bool ValidateLeakyRelu(const dom::MLOperand& aInput,
                       const dom::MLLeakyReluOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("leakyRelu is not implemented yet");
  return false;
}

bool ValidateLinear(const dom::MLOperand& aInput,
                    const dom::MLLinearOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("linear is not implemented yet");
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

bool ValidateAveragePool2d(const dom::MLOperand& aInput,
                           const dom::MLPool2dOptions& aOptions,
                           nsTArray<OperandDescriptor>& aOutputs,
                           ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("averagePool2d is not implemented yet");
  return false;
}

bool ValidateL2Pool2d(const dom::MLOperand& aInput,
                      const dom::MLPool2dOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("l2Pool2d is not implemented yet");
  return false;
}

bool ValidateMaxPool2d(const dom::MLOperand& aInput,
                       const dom::MLPool2dOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("maxPool2d is not implemented yet");
  return false;
}

bool ValidatePrelu(const dom::MLOperand& aInput, const dom::MLOperand& aSlope,
                   const dom::MLOperatorOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("prelu is not implemented yet");
  return false;
}

bool ValidateReduceL1(const dom::MLOperand& aInput,
                      const dom::MLReduceOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceL1 is not implemented yet");
  return false;
}

bool ValidateReduceL2(const dom::MLOperand& aInput,
                      const dom::MLReduceOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceL2 is not implemented yet");
  return false;
}

bool ValidateReduceLogSum(const dom::MLOperand& aInput,
                          const dom::MLReduceOptions& aOptions,
                          nsTArray<OperandDescriptor>& aOutputs,
                          ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceLogSum is not implemented yet");
  return false;
}

bool ValidateReduceLogSumExp(const dom::MLOperand& aInput,
                             const dom::MLReduceOptions& aOptions,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceLogSumExp is not implemented yet");
  return false;
}

bool ValidateReduceMax(const dom::MLOperand& aInput,
                       const dom::MLReduceOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceMax is not implemented yet");
  return false;
}

bool ValidateReduceMean(const dom::MLOperand& aInput,
                        const dom::MLReduceOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceMean is not implemented yet");
  return false;
}

bool ValidateReduceMin(const dom::MLOperand& aInput,
                       const dom::MLReduceOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceMin is not implemented yet");
  return false;
}

bool ValidateReduceProduct(const dom::MLOperand& aInput,
                           const dom::MLReduceOptions& aOptions,
                           nsTArray<OperandDescriptor>& aOutputs,
                           ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceProduct is not implemented yet");
  return false;
}

bool ValidateReduceSum(const dom::MLOperand& aInput,
                       const dom::MLReduceOptions& aOptions,
                       nsTArray<OperandDescriptor>& aOutputs,
                       ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceSum is not implemented yet");
  return false;
}

bool ValidateReduceSumSquare(const dom::MLOperand& aInput,
                             const dom::MLReduceOptions& aOptions,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("reduceSumSquare is not implemented yet");
  return false;
}

bool ValidateResample2d(const dom::MLOperand& aInput,
                        const dom::MLResample2dOptions& aOptions,
                        nsTArray<OperandDescriptor>& aOutputs,
                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("resample2d is not implemented yet");
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

bool ValidateSigmoid(const dom::MLOperand& aInput,
                     const dom::MLOperatorOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("sigmoid is not implemented yet");
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

bool ValidateSoftmax(const dom::MLOperand& aInput, uint32_t aAxis,
                     const dom::MLOperatorOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("softmax is not implemented yet");
  return false;
}

bool ValidateSoftplus(const dom::MLOperand& aInput,
                      const dom::MLOperatorOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("softplus is not implemented yet");
  return false;
}

bool ValidateSoftsign(const dom::MLOperand& aInput,
                      const dom::MLOperatorOptions& aOptions,
                      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("softsign is not implemented yet");
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

bool ValidateTanh(const dom::MLOperand& aInput,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("tanh is not implemented yet");
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

bool ValidateWhere(const dom::MLOperand& aCondition,
                   const dom::MLOperand& aTrueValue,
                   const dom::MLOperand& aFalseValue,
                   const dom::MLOperatorOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("where is not implemented yet");
  return false;
}
}  // namespace mozilla::webnn
