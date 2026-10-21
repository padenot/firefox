/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_IPC_WEBNNPARENTOPS_H_
#define DOM_WEBNN_IPC_WEBNNPARENTOPS_H_

#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/webnn/WebNNTypes.h"
#include "mozilla/webnn/webnn_engine_ops.h"
#include "nsTArray.h"

namespace mozilla::llama {
struct LlamaLibWrapper;
}

namespace mozilla::webnn {

enum class AddOperationResult {
  Ok,
  // The engine rejected the operation, see webnn_last_error_message().
  EngineError,
  // The engine returned a different number of outputs than the content process.
  OutputMismatch,
  // The operation refers to operands that do not exist yet, or redefines
  // operands: the content process is misbehaving.
  Invalid,
};

// Converts the IPDL operand indices of an operation to engine operand ids,
// remembering whether one of them was invalid. Also owns the temporary arrays
// the FFI structs point to.
class OperandResolver {
 public:
  explicit OperandResolver(nsTArray<Maybe<uint32_t>>& aOperands)
      : mOperands(aOperands) {}

  void Fail() { mValid = false; }

  WebnnOperandId Operand(uint32_t aIndex) {
    if (aIndex >= mOperands.Length() || !mOperands[aIndex]) {
      Fail();
      return 0;
    }
    return *mOperands[aIndex];
  }

  WebnnOptionalOperand OptionalOperand(const Maybe<uint32_t>& aIndex) {
    if (!aIndex) {
      return {false, 0};
    }
    return {true, Operand(*aIndex)};
  }

  WebnnOperandSlice Operands(const nsTArray<uint32_t>& aIndices) {
    nsTArray<uint32_t>& ids = *mStorage.AppendElement();
    for (uint32_t index : aIndices) {
      ids.AppendElement(Operand(index));
    }
    return {ids.Elements(), ids.Length()};
  }

  // Converts binding enums, which have the same values as the FFI ones.
  template <typename FfiSlice, typename Enum>
  FfiSlice Enums(const Maybe<nsTArray<Enum>>& aValues) {
    if (!aValues) {
      return {false, nullptr, 0};
    }
    nsTArray<uint32_t>& values = *mStorage.AppendElement();
    for (Enum value : *aValues) {
      values.AppendElement(static_cast<uint32_t>(value));
    }
    return {true, values.Elements(), values.Length()};
  }

  // Calls an engine operator function, then records the ids of its outputs.
  template <typename Args>
  AddOperationResult Call(WebnnStatus (*aFunction)(WebnnGraphBuilder*,
                                                   const Args*, uint32_t*,
                                                   uintptr_t, uintptr_t*),
                          WebnnGraphBuilder* aBuilder, const Args& aArgs,
                          Span<const uint32_t> aOutputs) {
    if (!mValid) {
      return AddOperationResult::Invalid;
    }
    for (size_t i = 0; i < aOutputs.Length(); i++) {
      if (aOutputs[i] >= mOperands.Length() || mOperands[aOutputs[i]]) {
        return AddOperationResult::Invalid;
      }
      for (size_t j = 0; j < i; j++) {
        if (aOutputs[j] == aOutputs[i]) {
          return AddOperationResult::Invalid;
        }
      }
    }
    nsTArray<uint32_t> ids;
    ids.SetLength(aOutputs.Length());
    uintptr_t length = 0;
    if (aFunction(aBuilder, &aArgs, ids.Elements(), ids.Length(), &length) !=
        WebnnStatus_Success) {
      return AddOperationResult::EngineError;
    }
    if (length != aOutputs.Length()) {
      return AddOperationResult::OutputMismatch;
    }
    for (size_t i = 0; i < aOutputs.Length(); i++) {
      mOperands[aOutputs[i]] = Some(ids[i]);
    }
    return AddOperationResult::Ok;
  }

 private:
  nsTArray<Maybe<uint32_t>>& mOperands;
  nsTArray<nsTArray<uint32_t>> mStorage;
  bool mValid = true;
};

inline WebnnStr ToFfi(const nsCString& aValue) {
  return {aValue.get(), aValue.Length()};
}

inline WebnnOptionalF64 ToFfi(const Maybe<double>& aValue) {
  return {aValue.isSome(), aValue.valueOr(0.0)};
}

inline WebnnU32Slice ToFfi(const nsTArray<uint32_t>& aValue) {
  return {aValue.Elements(), aValue.Length()};
}

inline WebnnOptionalU32Slice ToFfi(const Maybe<nsTArray<uint32_t>>& aValue) {
  if (!aValue) {
    return {false, nullptr, 0};
  }
  return {true, aValue->Elements(), aValue->Length()};
}

inline WebnnOptionalF32Slice ToFfi(const Maybe<nsTArray<float>>& aValue) {
  if (!aValue) {
    return {false, nullptr, 0};
  }
  return {true, aValue->Elements(), aValue->Length()};
}

// Adds an operator, other than input and constant, to aBuilder. aOperands maps
// IPDL operand indices to engine operand ids. Generated from WebNN.webidl.
AddOperationResult AddOperation(llama::LlamaLibWrapper* aLib,
                                WebnnGraphBuilder* aBuilder,
                                const WebNNOperation& aOperation,
                                nsTArray<Maybe<uint32_t>>& aOperands);

}  // namespace mozilla::webnn

#endif  // DOM_WEBNN_IPC_WEBNNPARENTOPS_H_
