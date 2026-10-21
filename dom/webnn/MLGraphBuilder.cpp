/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/MLGraphBuilder.h"

#include "mozilla/dom/MLContext.h"
#include "mozilla/dom/MLGraph.h"
#include "mozilla/dom/MLTensor.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/webnn/WebNNChild.h"
#include "mozilla/webnn/WebNNUtils.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MLGraphBuilder, mContext)
NS_IMPL_CYCLE_COLLECTING_ADDREF(MLGraphBuilder)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MLGraphBuilder)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MLGraphBuilder)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

static constexpr size_t kMaxRank = 8;

MLGraphBuilder::MLGraphBuilder(MLContext* aContext) : mContext(aContext) {}

/* static */
already_AddRefed<MLGraphBuilder> MLGraphBuilder::Constructor(
    const GlobalObject& aGlobal, MLContext& aContext, ErrorResult& aRv) {
  if (aContext.IsLost()) {
    aRv.ThrowInvalidStateError("The context is lost");
    return nullptr;
  }
  return do_AddRef(new MLGraphBuilder(&aContext));
}

JSObject* MLGraphBuilder::WrapObject(JSContext* aCx,
                                     JS::Handle<JSObject*> aGivenProto) {
  return MLGraphBuilder_Binding::Wrap(aCx, this, aGivenProto);
}

bool MLGraphBuilder::CheckState(ErrorResult& aRv) const {
  if (mBuilt) {
    aRv.ThrowInvalidStateError("The graph has already been built");
    return false;
  }
  if (mContext->IsLost()) {
    aRv.ThrowInvalidStateError("The context is lost");
    return false;
  }
  return true;
}

bool MLGraphBuilder::CheckOperand(const MLOperand& aOperand,
                                  ErrorResult& aRv) const {
  if (aOperand.Builder() != this) {
    aRv.ThrowTypeError("The operand belongs to a different builder");
    return false;
  }
  return true;
}

bool MLGraphBuilder::CheckOperand(
    const Optional<OwningNonNull<MLOperand>>& aOperand,
    ErrorResult& aRv) const {
  return !aOperand.WasPassed() || CheckOperand(aOperand.Value(), aRv);
}

bool MLGraphBuilder::CheckOperands(
    const Sequence<OwningNonNull<MLOperand>>& aOperands,
    ErrorResult& aRv) const {
  for (const MLOperand& operand : aOperands) {
    if (!CheckOperand(operand, aRv)) {
      return false;
    }
  }
  return true;
}

/* static */
Maybe<uint32_t> MLGraphBuilder::OptionalIndex(
    const Optional<OwningNonNull<MLOperand>>& aOperand) {
  return aOperand.WasPassed() ? Some(aOperand.Value().Index()) : Nothing();
}

/* static */
nsTArray<uint32_t> MLGraphBuilder::Indices(
    const Sequence<OwningNonNull<MLOperand>>& aOperands) {
  nsTArray<uint32_t> indices(aOperands.Length());
  for (const MLOperand& operand : aOperands) {
    indices.AppendElement(operand.Index());
  }
  return indices;
}

/* static */
nsTArray<uint32_t> MLGraphBuilder::OutputIndices(
    const nsTArray<RefPtr<MLOperand>>& aOperands) {
  nsTArray<uint32_t> indices(aOperands.Length());
  for (const RefPtr<MLOperand>& operand : aOperands) {
    indices.AppendElement(operand->Index());
  }
  return indices;
}

static bool CheckDescriptor(const MLOperandDescriptor& aDescriptor,
                            ErrorResult& aRv) {
  if (aDescriptor.mShape.Length() > kMaxRank) {
    aRv.ThrowTypeError("The rank is too large");
    return false;
  }
  for (uint32_t dim : aDescriptor.mShape) {
    if (!dim) {
      aRv.ThrowTypeError("Dimensions must be positive");
      return false;
    }
  }
  Maybe<size_t> byteLength =
      webnn::ByteLength(aDescriptor.mDataType, aDescriptor.mShape);
  if (!byteLength || *byteLength > MLContext::kMaxTensorByteLength) {
    aRv.ThrowTypeError("The operand is too large");
    return false;
  }
  return true;
}

already_AddRefed<MLOperand> MLGraphBuilder::AddOperand(
    MLOperand::Kind aKind, MLOperandDataType aDataType,
    nsTArray<uint32_t>&& aShape) {
  uint32_t index = mOperands.Length();
  mOperands.AppendElement(webnn::WebNNOperandDesc(aDataType, aShape.Clone()));
  return do_AddRef(
      new MLOperand(this, index, aKind, aDataType, std::move(aShape)));
}

nsTArray<RefPtr<MLOperand>> MLGraphBuilder::AddOutputs(
    nsTArray<webnn::OperandDescriptor>&& aDescriptors) {
  nsTArray<RefPtr<MLOperand>> outputs(aDescriptors.Length());
  for (webnn::OperandDescriptor& descriptor : aDescriptors) {
    outputs.AppendElement(AddOperand(MLOperand::Kind::Output,
                                     descriptor.mDataType,
                                     std::move(descriptor.mShape)));
  }
  return outputs;
}

already_AddRefed<MLOperand> MLGraphBuilder::AddConstant(
    MLOperandDataType aDataType, nsTArray<uint32_t>&& aShape,
    Span<const uint8_t> aData) {
  uint32_t constantIndex = mConstants.Length();
  mConstants.AppendElement(mozilla::ipc::BigBuffer(aData));
  RefPtr<MLOperand> operand =
      AddOperand(MLOperand::Kind::Constant, aDataType, std::move(aShape));
  mOperations.AppendElement(
      webnn::WebNNConstantOp(constantIndex, operand->Index()));
  return operand.forget();
}

already_AddRefed<MLOperand> MLGraphBuilder::Input(
    const nsAString& aName, const MLOperandDescriptor& aDescriptor,
    ErrorResult& aRv) {
  if (!CheckState(aRv)) {
    return nullptr;
  }
  if (aName.IsEmpty()) {
    aRv.ThrowTypeError("The input name must not be empty");
    return nullptr;
  }
  NS_ConvertUTF16toUTF8 name(aName);
  if (mInputNames.Contains(name)) {
    aRv.ThrowTypeError("Duplicate input name: "_ns + name);
    return nullptr;
  }
  if (!CheckDescriptor(aDescriptor, aRv)) {
    return nullptr;
  }

  mInputNames.Insert(name);
  RefPtr<MLOperand> operand =
      AddOperand(MLOperand::Kind::Input, aDescriptor.mDataType,
                 webnn::ToShape(aDescriptor.mShape));
  mOperations.AppendElement(webnn::WebNNInputOp(name, operand->Index()));
  return operand.forget();
}

already_AddRefed<MLOperand> MLGraphBuilder::Constant(
    const MLOperandDescriptor& aDescriptor,
    const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aBuffer,
    ErrorResult& aRv) {
  if (!CheckState(aRv) || !CheckDescriptor(aDescriptor, aRv)) {
    return nullptr;
  }
  size_t byteLength =
      *webnn::ByteLength(aDescriptor.mDataType, aDescriptor.mShape);

  RefPtr<MLOperand> operand;
  ProcessTypedArraysFixed(aBuffer, [&](const Span<uint8_t>& aData) {
    if (aData.Length() == byteLength) {
      operand = AddConstant(aDescriptor.mDataType,
                            webnn::ToShape(aDescriptor.mShape), aData);
    }
  });
  if (!operand) {
    aRv.ThrowTypeError("The buffer size does not match the descriptor");
    return nullptr;
  }
  return operand.forget();
}

template <typename T>
static Span<const uint8_t> AsBytes(const T& aValue) {
  return Span(reinterpret_cast<const uint8_t*>(&aValue), sizeof(T));
}

already_AddRefed<MLOperand> MLGraphBuilder::Constant(
    MLOperandDataType aDataType, double aValue, ErrorResult& aRv) {
  if (!CheckState(aRv)) {
    return nullptr;
  }

  nsTArray<uint32_t> scalar;
  switch (aDataType) {
    case MLOperandDataType::Float32: {
      float value = static_cast<float>(aValue);
      return AddConstant(aDataType, std::move(scalar), AsBytes(value));
    }
    case MLOperandDataType::Int32: {
      int32_t value = static_cast<int32_t>(aValue);
      return AddConstant(aDataType, std::move(scalar), AsBytes(value));
    }
    case MLOperandDataType::Uint32: {
      uint32_t value = static_cast<uint32_t>(aValue);
      return AddConstant(aDataType, std::move(scalar), AsBytes(value));
    }
    case MLOperandDataType::Int64: {
      int64_t value = static_cast<int64_t>(aValue);
      return AddConstant(aDataType, std::move(scalar), AsBytes(value));
    }
    case MLOperandDataType::Uint64: {
      uint64_t value = static_cast<uint64_t>(aValue);
      return AddConstant(aDataType, std::move(scalar), AsBytes(value));
    }
    case MLOperandDataType::Int8: {
      int8_t value = static_cast<int8_t>(aValue);
      return AddConstant(aDataType, std::move(scalar), AsBytes(value));
    }
    case MLOperandDataType::Uint8: {
      uint8_t value = static_cast<uint8_t>(aValue);
      return AddConstant(aDataType, std::move(scalar), AsBytes(value));
    }
    case MLOperandDataType::Float16:
      break;
  }
  aRv.ThrowNotSupportedError("float16 scalar constants are not supported yet");
  return nullptr;
}

already_AddRefed<MLOperand> MLGraphBuilder::Constant(MLTensor& aTensor,
                                                     ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("Constant tensors are not supported yet");
  return nullptr;
}

already_AddRefed<Promise> MLGraphBuilder::Build(
    const Record<nsString, OwningNonNull<MLOperand>>& aOutputs,
    ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mContext->GetParentObject(), aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  ErrorResult rv;
  if (!CheckState(rv)) {
    promise->MaybeReject(std::move(rv));
    return promise.forget();
  }
  if (aOutputs.Entries().IsEmpty()) {
    promise->MaybeRejectWithTypeError("The graph must have outputs");
    return promise.forget();
  }

  nsTArray<webnn::WebNNNamedOperand> outputs;
  MLGraph::OperandMap outputInfo;
  for (const auto& entry : aOutputs.Entries()) {
    const MLOperand& operand = entry.mValue;
    if (operand.Builder() != this) {
      promise->MaybeRejectWithTypeError(
          "An output belongs to a different builder");
      return promise.forget();
    }
    if (operand.GetKind() != MLOperand::Kind::Output) {
      promise->MaybeRejectWithTypeError(
          "An output cannot be an input or a constant");
      return promise.forget();
    }
    NS_ConvertUTF16toUTF8 name(entry.mKey);
    outputs.AppendElement(webnn::WebNNNamedOperand(name, operand.Index()));
    outputInfo.InsertOrUpdate(
        name,
        MLGraph::OperandInfo{operand.DataType(), operand.Shape().Clone()});
  }

  MLGraph::OperandMap inputInfo;
  for (const webnn::WebNNOperation& op : mOperations) {
    if (op.type() == webnn::WebNNOperation::TWebNNInputOp) {
      const webnn::WebNNInputOp& input = op.get_WebNNInputOp();
      const webnn::WebNNOperandDesc& desc = mOperands[input.output()];
      inputInfo.InsertOrUpdate(
          input.name(),
          MLGraph::OperandInfo{desc.dataType(), desc.shape().Clone()});
    }
  }

  mBuilt = true;
  uint32_t graphId = mContext->NextGraphId();
  RefPtr<MLContext> context = mContext;
  mContext->Actor()->SendBuild(
      graphId,
      webnn::WebNNGraphDesc(std::move(mOperands), std::move(mOperations),
                            std::move(outputs)),
      std::move(mConstants),
      [promise, context, graphId, inputInfo = std::move(inputInfo),
       outputInfo = std::move(outputInfo)](const nsCString& aError) mutable {
        if (!aError.IsEmpty()) {
          promise->MaybeRejectWithOperationError(aError);
          return;
        }
        RefPtr<MLGraph> graph = new MLGraph(
            context, graphId, std::move(inputInfo), std::move(outputInfo));
        promise->MaybeResolve(graph);
      },
      [promise](mozilla::ipc::ResponseRejectReason) {
        promise->MaybeRejectWithInvalidStateError("The context is lost");
      });
  return promise.forget();
}

}  // namespace mozilla::dom
