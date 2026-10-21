/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "mozilla/CheckedInt.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/MLContext.h"
#include "mozilla/webnn/WebNNUtils.h"
#include "nsReadableUtils.h"

namespace mozilla::webnn {

bool IsDataTypeIn(MLOperandDataType aType,
                  Span<const MLOperandDataType> aAllowed) {
  return std::find(aAllowed.begin(), aAllowed.end(), aType) != aAllowed.end();
}

static bool IsUnsafeLabelChar(char16_t aChar) {
  return aChar < 0x20 || (aChar >= 0x7F && aChar <= 0x9F) ||
         (aChar >= 0x202A && aChar <= 0x202E) ||
         (aChar >= 0x2066 && aChar <= 0x2069);
}

void ThrowOpTypeError(ErrorResult& aRv, const nsAString& aLabel,
                      const char* aOp, const char* aMessage) {
  nsAutoCString message;
  if (!aLabel.IsEmpty()) {
    nsAutoString label;
    for (char16_t c : aLabel.View()) {
      if (!IsUnsafeLabelChar(c)) {
        label.Append(c);
      }
    }
    message.Append('[');
    AppendUTF16toUTF8(label, message);
    message.AppendLiteral("] ");
  }
  message.Append(aOp);
  message.AppendLiteral(": ");
  message.Append(aMessage);
  aRv.ThrowTypeError(message);
}

bool CheckDimensions(MLOperandDataType aType, Span<const uint32_t> aShape) {
  if (aShape.Length() > kMaxRank) {
    return false;
  }
  CheckedInt<int64_t> elementCount = 1;
  for (uint32_t dim : aShape) {
    if (!IsValidDimension(dim)) {
      return false;
    }
    elementCount *= dim;
  }
  if (!elementCount.isValid() || !IsValidDimension(elementCount.value())) {
    return false;
  }
  Maybe<size_t> byteLength = ByteLength(aType, aShape);
  return byteLength && *byteLength <= dom::MLContext::kMaxTensorByteLength;
}

bool CreateOperandDescriptor(MLOperandDataType aType,
                             nsTArray<uint32_t>&& aShape,
                             const nsAString& aLabel, const char* aOp,
                             nsTArray<OperandDescriptor>& aOutputs,
                             ErrorResult& aRv) {
  if (!CheckDimensions(aType, aShape)) {
    ThrowOpTypeError(aRv, aLabel, aOp, "invalid output dimensions");
    return false;
  }
  aOutputs.AppendElement(OperandDescriptor{aType, std::move(aShape)});
  return true;
}

void CopyOperand(const dom::MLOperand& aInput,
                 nsTArray<OperandDescriptor>& aOutputs) {
  aOutputs.AppendElement(
      OperandDescriptor{aInput.DataType(), aInput.Shape().Clone()});
}

Maybe<nsTArray<uint32_t>> BidirectionallyBroadcastShapes(
    Span<const uint32_t> aA, Span<const uint32_t> aB) {
  size_t rank = std::max(aA.Length(), aB.Length());
  nsTArray<uint32_t> result;
  result.SetLength(rank);
  for (size_t i = 0; i < rank; i++) {
    uint32_t a = i < aA.Length() ? aA[aA.Length() - 1 - i] : 1;
    uint32_t b = i < aB.Length() ? aB[aB.Length() - 1 - i] : 1;
    if (a != b && a != 1 && b != 1) {
      return Nothing();
    }
    result[rank - 1 - i] = std::max(a, b);
  }
  return Some(std::move(result));
}

bool IsUnidirectionallyBroadcastable(Span<const uint32_t> aFrom,
                                     Span<const uint32_t> aTo) {
  if (aFrom.Length() > aTo.Length()) {
    return false;
  }
  size_t offset = aTo.Length() - aFrom.Length();
  for (size_t i = 0; i < aFrom.Length(); i++) {
    if (aFrom[i] != aTo[offset + i] && aFrom[i] != 1) {
      return false;
    }
  }
  return true;
}

bool IsBlockwiseBroadcastable(Span<const uint32_t> aFrom,
                              Span<const uint32_t> aTo) {
  if (aFrom.Length() != aTo.Length()) {
    return false;
  }
  for (size_t i = 0; i < aTo.Length(); i++) {
    if (aTo[i] % aFrom[i]) {
      return false;
    }
  }
  return true;
}

bool AreValidAxes(Span<const uint32_t> aAxes, size_t aRank) {
  nsTArray<bool> seen;
  seen.SetLength(aRank);
  std::fill(seen.begin(), seen.end(), false);
  for (uint32_t axis : aAxes) {
    if (axis >= aRank || seen[axis]) {
      return false;
    }
    seen[axis] = true;
  }
  return true;
}

bool ShapeEquals(Span<const uint32_t> aShape,
                 std::initializer_list<uint64_t> aExpected) {
  if (aShape.Length() != aExpected.size()) {
    return false;
  }
  size_t i = 0;
  for (uint64_t dim : aExpected) {
    if (aShape[i++] != dim) {
      return false;
    }
  }
  return true;
}

Maybe<nsTArray<uint32_t>> CalculateReductionOutputSizes(
    Span<const uint32_t> aInputShape, const Maybe<Span<const uint32_t>>& aAxes,
    bool aKeepDimensions) {
  size_t rank = aInputShape.Length();
  if (aAxes && !AreValidAxes(*aAxes, rank)) {
    return Nothing();
  }
  nsTArray<bool> reduced;
  reduced.SetLength(rank);
  std::fill(reduced.begin(), reduced.end(), !aAxes);
  if (aAxes) {
    for (uint32_t axis : *aAxes) {
      reduced[axis] = true;
    }
  }
  nsTArray<uint32_t> result;
  for (size_t i = 0; i < rank; i++) {
    if (!reduced[i]) {
      result.AppendElement(aInputShape[i]);
    } else if (aKeepDimensions) {
      result.AppendElement(1);
    }
  }
  return Some(std::move(result));
}

// ConvertToFloat with a format of aMantissaBits explicit significand bits and
// normal exponents in [aMinExponent, aMaxExponent]. Rounds to nearest, ties to
// even, with 2^(aMaxExponent + 1) as the upper bound that maps to Infinity.
static double ConvertToFloat(double aX, int aMantissaBits, int aMinExponent,
                             int aMaxExponent) {
  if (std::isnan(aX) || std::isinf(aX)) {
    return aX;
  }
  int exponent;
  std::frexp(aX, &exponent);
  int quantumExponent = std::max(exponent - 1, aMinExponent) - aMantissaBits;
  double y = std::ldexp(std::nearbyint(std::ldexp(aX, -quantumExponent)),
                        quantumExponent);
  if (std::fabs(y) >= std::ldexp(1.0, aMaxExponent + 1)) {
    return std::copysign(std::numeric_limits<double>::infinity(), aX);
  }
  return y;
}

static double ConvertToInt(double aX, int aBitLength, bool aSigned) {
  double lowerBound = aSigned ? -std::ldexp(1.0, aBitLength - 1) : 0.0;
  double upperBound = aSigned ? std::ldexp(1.0, aBitLength - 1) - 1
                              : std::ldexp(1.0, aBitLength) - 1;
  if (std::isnan(aX)) {
    return 0.0;
  }
  double x = std::min(std::max(aX, lowerBound), upperBound);
  return std::nearbyint(x) + 0.0;
}

double CastNumber(double aX, MLOperandDataType aType) {
  switch (aType) {
    case MLOperandDataType::Float32:
      return ConvertToFloat(aX, 23, -126, 127);
    case MLOperandDataType::Float16:
      return ConvertToFloat(aX, 10, -14, 15);
    case MLOperandDataType::Int64:
      return ConvertToInt(aX, 64, true);
    case MLOperandDataType::Uint64:
      return ConvertToInt(aX, 64, false);
    case MLOperandDataType::Int32:
      return ConvertToInt(aX, 32, true);
    case MLOperandDataType::Uint32:
      // The spec says "signed" for uint32.
      return ConvertToInt(aX, 32, true);
    case MLOperandDataType::Int8:
      return ConvertToInt(aX, 8, true);
    case MLOperandDataType::Uint8:
      return ConvertToInt(aX, 8, false);
  }
  MOZ_CRASH("Unexpected MLOperandDataType");
}

}  // namespace mozilla::webnn
