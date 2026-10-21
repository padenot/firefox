/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_VALIDATION_VALIDATIONUTILS_H_
#define DOM_WEBNN_VALIDATION_VALIDATIONUTILS_H_

#include <array>
#include <initializer_list>

#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/dom/MLOperand.h"
#include "mozilla/dom/WebNNBinding.h"
#include "mozilla/webnn/WebNNValidation.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla {
class ErrorResult;
}

namespace mozilla::webnn {

using dom::MLOperandDataType;

// Implementation-defined rank bound for "check dimensions".
constexpr size_t kMaxRank = 8;

constexpr MLOperandDataType kFloatTypes[] = {MLOperandDataType::Float32,
                                             MLOperandDataType::Float16};
constexpr MLOperandDataType kFloatAndSignedIntTypes[] = {
    MLOperandDataType::Float32, MLOperandDataType::Float16,
    MLOperandDataType::Int64, MLOperandDataType::Int32,
    MLOperandDataType::Int8};
constexpr MLOperandDataType kFloatAndWideIntTypes[] = {
    MLOperandDataType::Float32, MLOperandDataType::Float16,
    MLOperandDataType::Int32,   MLOperandDataType::Uint32,
    MLOperandDataType::Int64,   MLOperandDataType::Uint64};
constexpr MLOperandDataType kIndicesTypes[] = {MLOperandDataType::Int32,
                                               MLOperandDataType::Uint32,
                                               MLOperandDataType::Int64};
constexpr MLOperandDataType kQuantizedTypes[] = {
    MLOperandDataType::Uint8, MLOperandDataType::Int8,
    MLOperandDataType::Uint32, MLOperandDataType::Int32};

bool IsDataTypeIn(MLOperandDataType aType,
                  Span<const MLOperandDataType> aAllowed);

// Throws a TypeError of the form "[label] op: message". Control and
// bidirectional formatting characters are stripped from the label.
void ThrowOpTypeError(ErrorResult& aRv, const nsAString& aLabel,
                      const char* aOp, const char* aMessage);

// https://webmachinelearning.github.io/webnn/#valid-dimension
inline bool IsValidDimension(int64_t aValue) {
  return aValue > 0 && aValue <= INT32_MAX;
}

// https://webmachinelearning.github.io/webnn/#valid-tensor-count
inline bool IsValidTensorCount(uint64_t aCount) {
  return aCount > 0 && aCount <= 8192;
}

// https://webmachinelearning.github.io/webnn/#mloperanddescriptor-check-dimensions
bool CheckDimensions(MLOperandDataType aType, Span<const uint32_t> aShape);

// https://webmachinelearning.github.io/webnn/#create-an-mloperanddescriptor
// Appends the descriptor to aOutputs, or throws a TypeError.
bool CreateOperandDescriptor(MLOperandDataType aType,
                             nsTArray<uint32_t>&& aShape,
                             const nsAString& aLabel, const char* aOp,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv);

// https://webmachinelearning.github.io/webnn/#copy-an-mloperand
void CopyOperand(const dom::MLOperand& aInput,
                 nsTArray<OperandDescriptor>& aOutputs);

// https://webmachinelearning.github.io/webnn/#bidirectionally-broadcasting
Maybe<nsTArray<uint32_t>> BidirectionallyBroadcastShapes(
    Span<const uint32_t> aA, Span<const uint32_t> aB);

// https://webmachinelearning.github.io/webnn/#unidirectionally-broadcastable
bool IsUnidirectionallyBroadcastable(Span<const uint32_t> aFrom,
                                     Span<const uint32_t> aTo);

// https://webmachinelearning.github.io/webnn/#blockwise-broadcastable
bool IsBlockwiseBroadcastable(Span<const uint32_t> aFrom,
                              Span<const uint32_t> aTo);

// True if every item of aAxes is in [0, aRank) and there are no duplicates.
bool AreValidAxes(Span<const uint32_t> aAxes, size_t aRank);

bool ShapeEquals(Span<const uint32_t> aShape,
                 std::initializer_list<uint64_t> aExpected);

// https://webmachinelearning.github.io/webnn/#mlgraphbuilder-calculate-reduction-output-sizes
Maybe<nsTArray<uint32_t>> CalculateReductionOutputSizes(
    Span<const uint32_t> aInputShape, const Maybe<Span<const uint32_t>>& aAxes,
    bool aKeepDimensions);

// https://webmachinelearning.github.io/webnn/#algorithms-casting
double CastNumber(double aX, MLOperandDataType aType);

// Resolves an optional fixed-size list option (padding, strides, ...) to
// aDefault if absent. Returns false if present with the wrong size.
template <size_t N>
bool ResolveListOption(const dom::Optional<dom::Sequence<uint32_t>>& aList,
                       uint32_t aDefault, std::array<uint32_t, N>& aOut) {
  if (!aList.WasPassed()) {
    aOut.fill(aDefault);
    return true;
  }
  if (aList.Value().Length() != N) {
    return false;
  }
  for (size_t i = 0; i < N; i++) {
    aOut[i] = aList.Value()[i];
  }
  return true;
}

}  // namespace mozilla::webnn

#endif  // DOM_WEBNN_VALIDATION_VALIDATIONUTILS_H_
