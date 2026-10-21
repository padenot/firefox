/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ValidationUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/webnn/WebNNValidation.h"

namespace mozilla::webnn {

using dom::MLOperand;

// Activations whose method steps only check the input data type against the
// allowed set, then copy the input operand. Numeric options (alpha, beta) are
// cast to the input data type, which cannot fail.
static bool ValidateActivation(const char* aOp, const MLOperand& aInput,
                               Span<const MLOperandDataType> aAllowedDataTypes,
                               const nsAString& aLabel,
                               nsTArray<OperandDescriptor>& aOutputs,
                               ErrorResult& aRv) {
  if (!IsDataTypeIn(aInput.DataType(), aAllowedDataTypes)) {
    ThrowOpTypeError(aRv, aLabel, aOp, "unsupported data type");
    return false;
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

#define WEBNN_ACTIVATION(Name, op, OptionsType, allowed)                      \
  bool Validate##Name(                                                        \
      const MLOperand& aInput, const dom::OptionsType& aOptions,              \
      nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {              \
    return ValidateActivation(op, aInput, allowed, aOptions.mLabel, aOutputs, \
                              aRv);                                           \
  }

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-relu-method
WEBNN_ACTIVATION(Relu, "relu", MLOperatorOptions, kFloatAndSignedIntTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-sigmoid-method
WEBNN_ACTIVATION(Sigmoid, "sigmoid", MLOperatorOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-tanh-method
WEBNN_ACTIVATION(Tanh, "tanh", MLOperatorOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-softplus-method
WEBNN_ACTIVATION(Softplus, "softplus", MLOperatorOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-softsign-method
WEBNN_ACTIVATION(Softsign, "softsign", MLOperatorOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-gelu-method
WEBNN_ACTIVATION(Gelu, "gelu", MLOperatorOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-hard-swish
WEBNN_ACTIVATION(HardSwish, "hardSwish", MLOperatorOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-elu
WEBNN_ACTIVATION(Elu, "elu", MLEluOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-leakyrelu
WEBNN_ACTIVATION(LeakyRelu, "leakyRelu", MLLeakyReluOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-hard-sigmoid
WEBNN_ACTIVATION(HardSigmoid, "hardSigmoid", MLHardSigmoidOptions, kFloatTypes)
// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-linear
WEBNN_ACTIVATION(Linear, "linear", MLLinearOptions, kFloatTypes)

#undef WEBNN_ACTIVATION

// https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-softmax-method
bool ValidateSoftmax(const MLOperand& aInput, uint32_t aAxis,
                     const dom::MLOperatorOptions& aOptions,
                     nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv) {
  if (!IsDataTypeIn(aInput.DataType(), kFloatTypes)) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "softmax", "unsupported data type");
    return false;
  }
  if (aAxis >= aInput.Shape().Length()) {
    ThrowOpTypeError(aRv, aOptions.mLabel, "softmax",
                     "axis must be less than the input rank");
    return false;
  }
  CopyOperand(aInput, aOutputs);
  return true;
}

}  // namespace mozilla::webnn
