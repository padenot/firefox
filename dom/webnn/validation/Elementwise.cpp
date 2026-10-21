/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <limits>

#include "ValidationUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

using dom::MLOperand;

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-binary
static bool ValidateElementwiseBinary(const char* aOp, const MLOperand& aA,
                                      const MLOperand& aB,
                                      const dom::MLOperatorOptions& aOptions,
                                      nsTArray<OperandDescriptor>& aOutputs,
                                      ErrorResult& aRv) {
  if (aA.DataType() != aB.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "operands must have the same data type");
    return false;
  }
  Maybe<nsTArray<uint32_t>> shape =
      BidirectionallyBroadcastShapes(aA.Shape(), aB.Shape());
  if (!shape) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "shapes are not broadcastable");
    return false;
  }
  return CreateOperandDescriptor(aA.DataType(), shape.extract(),
                                 aOptions.mLabel, aOp, aOutputs, aRv);
}

#define WEBNN_ELEMENTWISE_BINARY(Name, op)                                 \
  bool Validate##Name(const MLOperand& aA, const MLOperand& aB,            \
                      const dom::MLOperatorOptions& aOptions,              \
                      nsTArray<OperandDescriptor>& aOutputs,               \
                      ErrorResult& aRv) {                                  \
    return ValidateElementwiseBinary(op, aA, aB, aOptions, aOutputs, aRv); \
  }

WEBNN_ELEMENTWISE_BINARY(Add, "add")
WEBNN_ELEMENTWISE_BINARY(Sub, "sub")
WEBNN_ELEMENTWISE_BINARY(Mul, "mul")
WEBNN_ELEMENTWISE_BINARY(Div, "div")
WEBNN_ELEMENTWISE_BINARY(Max, "max")
WEBNN_ELEMENTWISE_BINARY(Min, "min")
WEBNN_ELEMENTWISE_BINARY(Pow, "pow")

#undef WEBNN_ELEMENTWISE_BINARY

enum class LogicalInputs { Any, Uint8, Float };

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-logical
static bool ValidateElementwiseLogical(const char* aOp, LogicalInputs aInputs,
                                       const MLOperand& aA, const MLOperand* aB,
                                       const dom::MLOperatorOptions& aOptions,
                                       nsTArray<OperandDescriptor>& aOutputs,
                                       ErrorResult& aRv) {
  if (aInputs == LogicalInputs::Uint8 &&
      aA.DataType() != MLOperandDataType::Uint8) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "input must be uint8");
    return false;
  }
  if (aInputs == LogicalInputs::Float &&
      !IsDataTypeIn(aA.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                     "input must be float32 or float16");
    return false;
  }
  nsTArray<uint32_t> shape;
  if (aB) {
    if (aA.DataType() != aB->DataType()) {
      ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                       "operands must have the same data type");
      return false;
    }
    Maybe<nsTArray<uint32_t>> broadcast =
        BidirectionallyBroadcastShapes(aA.Shape(), aB->Shape());
    if (!broadcast) {
      ThrowOpTypeError(aRv, aOptions.mLabel, aOp,
                       "shapes are not broadcastable");
      return false;
    }
    shape = broadcast.extract();
  } else {
    shape = aA.Shape().Clone();
  }
  return CreateOperandDescriptor(MLOperandDataType::Uint8, std::move(shape),
                                 aOptions.mLabel, aOp, aOutputs, aRv);
}

#define WEBNN_ELEMENTWISE_LOGICAL_BINARY(Name, op, inputs)                \
  bool Validate##Name(const MLOperand& aA, const MLOperand& aB,           \
                      const dom::MLOperatorOptions& aOptions,             \
                      nsTArray<OperandDescriptor>& aOutputs,              \
                      ErrorResult& aRv) {                                 \
    return ValidateElementwiseLogical(op, LogicalInputs::inputs, aA, &aB, \
                                      aOptions, aOutputs, aRv);           \
  }

#define WEBNN_ELEMENTWISE_LOGICAL_UNARY(Name, op, inputs)                     \
  bool Validate##Name(                                                        \
      const MLOperand& aA, const dom::MLOperatorOptions& aOptions,            \
      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {              \
    return ValidateElementwiseLogical(op, LogicalInputs::inputs, aA, nullptr, \
                                      aOptions, aOutputs, aRv);               \
  }

WEBNN_ELEMENTWISE_LOGICAL_BINARY(Equal, "equal", Any)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(NotEqual, "notEqual", Any)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(Greater, "greater", Any)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(GreaterOrEqual, "greaterOrEqual", Any)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(Lesser, "lesser", Any)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(LesserOrEqual, "lesserOrEqual", Any)
WEBNN_ELEMENTWISE_LOGICAL_UNARY(LogicalNot, "logicalNot", Uint8)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(LogicalAnd, "logicalAnd", Uint8)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(LogicalOr, "logicalOr", Uint8)
WEBNN_ELEMENTWISE_LOGICAL_BINARY(LogicalXor, "logicalXor", Uint8)
WEBNN_ELEMENTWISE_LOGICAL_UNARY(IsNaN, "isNaN", Float)
WEBNN_ELEMENTWISE_LOGICAL_UNARY(IsInfinite, "isInfinite", Float)

#undef WEBNN_ELEMENTWISE_LOGICAL_BINARY
#undef WEBNN_ELEMENTWISE_LOGICAL_UNARY

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-unary
static bool ValidateElementwiseUnary(
    const char* aOp, const MLOperand& aInput,
    const Maybe<Span<const MLOperandDataType>>& aAllowedDataTypes,
    const dom::MLOperatorOptions& aOptions,
    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (aAllowedDataTypes &&
      !IsDataTypeIn(aInput.DataType(), *aAllowedDataTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, aOp, "unsupported data type");
    return false;
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

#define WEBNN_ELEMENTWISE_UNARY(Name, op, allowed)                           \
  bool Validate##Name(                                                       \
      const MLOperand& aInput, const dom::MLOperatorOptions& aOptions,       \
      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {             \
    return ValidateElementwiseUnary(op, aInput, allowed, aOptions, aOutputs, \
                                    aRv);                                    \
  }

#define WEBNN_FLOAT Some(Span<const MLOperandDataType>(kFloatTypes))
#define WEBNN_FLOAT_SIGNED_INT \
  Some(Span<const MLOperandDataType>(kFloatAndSignedIntTypes))

WEBNN_ELEMENTWISE_UNARY(Abs, "abs", WEBNN_FLOAT_SIGNED_INT)
WEBNN_ELEMENTWISE_UNARY(Ceil, "ceil", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Cos, "cos", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Erf, "erf", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Exp, "exp", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Floor, "floor", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Identity, "identity", Nothing())
WEBNN_ELEMENTWISE_UNARY(Log, "log", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Neg, "neg", WEBNN_FLOAT_SIGNED_INT)
WEBNN_ELEMENTWISE_UNARY(Reciprocal, "reciprocal", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(RoundEven, "roundEven", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Sin, "sin", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Sign, "sign", WEBNN_FLOAT_SIGNED_INT)
WEBNN_ELEMENTWISE_UNARY(Sqrt, "sqrt", WEBNN_FLOAT)
WEBNN_ELEMENTWISE_UNARY(Tan, "tan", WEBNN_FLOAT)

#undef WEBNN_FLOAT
#undef WEBNN_FLOAT_SIGNED_INT
#undef WEBNN_ELEMENTWISE_UNARY

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-where
bool ValidateWhere(const MLOperand& aCondition, const MLOperand& aTrueValue,
                   const MLOperand& aFalseValue,
                   const dom::MLOperatorOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (aCondition.DataType() != MLOperandDataType::Uint8) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "where", "condition must be uint8");
    return false;
  }
  if (aFalseValue.DataType() != aTrueValue.DataType()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "where",
                     "trueValue and falseValue must have the same data type");
    return false;
  }
  Maybe<nsTArray<uint32_t>> shape =
      BidirectionallyBroadcastShapes(aTrueValue.Shape(), aFalseValue.Shape());
  if (!shape) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "where",
                     "trueValue and falseValue are not broadcastable");
    return false;
  }
  shape = BidirectionallyBroadcastShapes(aCondition.Shape(), *shape);
  if (!shape) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "where",
                     "condition is not broadcastable");
    return false;
  }
  return CreateOperandDescriptor(aTrueValue.DataType(), shape.extract(),
                                 aOptions.mLabel, "where", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-cast
// The steps copy the input operand, but the output has the target data type
// and goes through "create an MLOperandDescriptor".
bool ValidateCast(const MLOperand& aInput, MLOperandDataType aDataType,
                  const dom::MLOperatorOptions& aOptions,
                  nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  return CreateOperandDescriptor(aDataType, aInput.Shape().Clone(),
                                 aOptions.mLabel, "cast", aOutputs, aRv);
}

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-clamp
// The steps swap the defaults; a missing bound does not clamp, as in the
// member descriptions.
bool ValidateClamp(const MLOperand& aInput, const dom::MLClampOptions& aOptions,
                   nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  double minValue = CastNumber(
      aOptions.mMinValue.WasPassed() ? aOptions.mMinValue.Value() : -kInfinity,
      aInput.DataType());
  double maxValue = CastNumber(
      aOptions.mMaxValue.WasPassed() ? aOptions.mMaxValue.Value() : kInfinity,
      aInput.DataType());
  if (minValue > maxValue) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "clamp",
                     "minValue is greater than maxValue");
    return false;
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

}  // namespace mozilla::webnn
