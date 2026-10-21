/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/MLContext.h"

#include "mozilla/dom/MLGraph.h"
#include "mozilla/dom/MLTensor.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/ipc/BigBuffer.h"
#include "mozilla/webnn/WebNNChild.h"
#include "mozilla/webnn/WebNNUtils.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MLContext, mGlobal, mLostPromise)
NS_IMPL_CYCLE_COLLECTING_ADDREF(MLContext)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MLContext)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MLContext)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

MLContext::MLContext(nsIGlobalObject* aGlobal, bool aAccelerated,
                     Promise* aLostPromise)
    : mGlobal(aGlobal),
      mLostPromise(aLostPromise),
      mAccelerated(aAccelerated) {}

MLContext::~MLContext() {
  if (mActor) {
    mActor->Detach();
  }
}

/* static */
void MLContext::Create(nsIGlobalObject* aGlobal,
                       const MLContextOptions& aOptions, Promise* aPromise) {
  IgnoredErrorResult rv;
  RefPtr<Promise> lost = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    aPromise->MaybeRejectWithUnknownError("Could not create the context");
    return;
  }

  RefPtr<MLContext> context =
      new MLContext(aGlobal, aOptions.mAccelerated, lost);
  context->mActor = webnn::WebNNChild::Create(context);
  if (!context->mActor) {
    aPromise->MaybeRejectWithNotSupportedError("WebNN is not available");
    return;
  }

  RefPtr<Promise> promise = aPromise;
  context->mActor->SendCreateContext(
      webnn::WebNNContextOptions(aOptions.mPowerPreference,
                                 aOptions.mAccelerated),
      [promise, context](const nsCString& aError) {
        if (!aError.IsEmpty()) {
          context->OnContextLost(aError);
          promise->MaybeRejectWithNotSupportedError(aError);
          return;
        }
        promise->MaybeResolve(context);
      },
      [promise, context](mozilla::ipc::ResponseRejectReason) {
        context->OnContextLost("The WebNN process went away"_ns);
        promise->MaybeRejectWithNotSupportedError("WebNN is not available");
      });
}

JSObject* MLContext::WrapObject(JSContext* aCx,
                                JS::Handle<JSObject*> aGivenProto) {
  return MLContext_Binding::Wrap(aCx, this, aGivenProto);
}

void MLContext::OnContextLost(const nsACString& aMessage) {
  if (!mActor) {
    return;
  }
  RefPtr<webnn::WebNNChild> actor = std::move(mActor);
  actor->Detach();

  MLContextLostInfo info;
  info.mMessage.Construct(NS_ConvertUTF8toUTF16(aMessage));
  mLostPromise->MaybeResolve(info);
}

void MLContext::Destroy() { OnContextLost("The context was destroyed"_ns); }

static bool ValidateTensors(
    MLContext* aContext,
    const Record<nsString, OwningNonNull<MLTensor>>& aTensors,
    const MLGraph::OperandMap& aExpected,
    nsTArray<webnn::WebNNNamedTensor>& aOut, ErrorResult& aRv) {
  if (aTensors.Entries().Length() != aExpected.Count()) {
    aRv.ThrowTypeError("Wrong number of tensors");
    return false;
  }
  for (const auto& entry : aTensors.Entries()) {
    NS_ConvertUTF16toUTF8 name(entry.mKey);
    const MLGraph::OperandInfo* expected =
        aExpected.Lookup(name).DataPtrOrNull();
    if (!expected) {
      aRv.ThrowTypeError("Unknown tensor name: "_ns + name);
      return false;
    }
    const MLTensor& tensor = entry.mValue;
    if (tensor.Context() != aContext || tensor.IsDestroyed()) {
      aRv.ThrowTypeError("Invalid tensor: "_ns + name);
      return false;
    }
    if (tensor.DataType() != expected->mDataType ||
        tensor.Shape() != expected->mShape) {
      aRv.ThrowTypeError("Tensor descriptor mismatch: "_ns + name);
      return false;
    }
    aOut.AppendElement(webnn::WebNNNamedTensor(name, tensor.Id()));
  }
  return true;
}

void MLContext::Dispatch(
    MLGraph& aGraph, const Record<nsString, OwningNonNull<MLTensor>>& aInputs,
    const Record<nsString, OwningNonNull<MLTensor>>& aOutputs,
    ErrorResult& aRv) {
  if (IsLost()) {
    aRv.ThrowInvalidStateError("The context is lost");
    return;
  }
  if (aGraph.Context() != this || aGraph.IsDestroyed()) {
    aRv.ThrowTypeError("Invalid graph");
    return;
  }

  nsTArray<webnn::WebNNNamedTensor> inputs;
  nsTArray<webnn::WebNNNamedTensor> outputs;
  if (!ValidateTensors(this, aInputs, aGraph.Inputs(), inputs, aRv) ||
      !ValidateTensors(this, aOutputs, aGraph.Outputs(), outputs, aRv)) {
    return;
  }

  for (const auto& output : outputs) {
    for (const auto& other : outputs) {
      if (&output != &other && output.tensor() == other.tensor()) {
        aRv.ThrowTypeError("A tensor is used for multiple outputs");
        return;
      }
    }
    for (const auto& input : inputs) {
      if (output.tensor() == input.tensor()) {
        aRv.ThrowTypeError("A tensor is used as both input and output");
        return;
      }
    }
  }

  mActor->SendDispatch(aGraph.Id(), inputs, outputs);
}

already_AddRefed<Promise> MLContext::CreateTensor(
    const MLTensorDescriptor& aDescriptor, ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  if (IsLost()) {
    promise->MaybeRejectWithInvalidStateError("The context is lost");
    return promise.forget();
  }

  Maybe<size_t> byteLength =
      webnn::ByteLength(aDescriptor.mDataType, aDescriptor.mShape);
  if (!byteLength || *byteLength > kMaxTensorByteLength) {
    promise->MaybeRejectWithTypeError("Tensor is too large");
    return promise.forget();
  }
  for (uint32_t dim : aDescriptor.mShape) {
    if (!dim) {
      promise->MaybeRejectWithTypeError("Tensor dimensions must be positive");
      return promise.forget();
    }
  }

  uint32_t id = mNextTensorId++;
  nsTArray<uint32_t> shape = webnn::ToShape(aDescriptor.mShape);
  RefPtr<MLContext> self = this;
  mActor->SendCreateTensor(
      id, webnn::WebNNOperandDesc(aDescriptor.mDataType, shape.Clone()),
      aDescriptor.mReadable, aDescriptor.mWritable,
      [self, promise, id, dataType = aDescriptor.mDataType,
       shape = std::move(shape), byteLength = *byteLength,
       readable = aDescriptor.mReadable,
       writable = aDescriptor.mWritable](const nsCString& aError) mutable {
        if (!aError.IsEmpty()) {
          promise->MaybeRejectWithUnknownError(aError);
          return;
        }
        RefPtr<MLTensor> tensor =
            new MLTensor(self, id, dataType, std::move(shape), byteLength,
                         readable, writable);
        promise->MaybeResolve(tensor);
      },
      [promise](mozilla::ipc::ResponseRejectReason) {
        promise->MaybeRejectWithInvalidStateError("The context is lost");
      });
  return promise.forget();
}

already_AddRefed<Promise> MLContext::CreateExportableTensor(
    const MLTensorDescriptor& aDescriptor, webgpu::Device& aDevice,
    ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  promise->MaybeRejectWithNotSupportedError("WebGPU interop is not supported");
  return promise.forget();
}

already_AddRefed<Promise> MLContext::CreateConstantTensor(
    const MLOperandDescriptor& aDescriptor,
    const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aInputData,
    ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  promise->MaybeRejectWithNotSupportedError(
      "Constant tensors are not supported yet");
  return promise.forget();
}

already_AddRefed<Promise> MLContext::ReadTensor(MLTensor& aTensor,
                                                ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  if (IsLost()) {
    promise->MaybeRejectWithInvalidStateError("The context is lost");
    return promise.forget();
  }
  if (aTensor.Context() != this || aTensor.IsDestroyed()) {
    promise->MaybeRejectWithTypeError("Invalid tensor");
    return promise.forget();
  }
  if (!aTensor.Readable()) {
    promise->MaybeRejectWithTypeError("The tensor is not readable");
    return promise.forget();
  }

  nsCOMPtr<nsIGlobalObject> global = mGlobal;
  mActor->SendReadTensor(
      aTensor.Id(),
      [promise, global](webnn::WebNNReadTensorResult&& aResult) {
        if (aResult.type() == webnn::WebNNReadTensorResult::TnsCString) {
          promise->MaybeRejectWithUnknownError(aResult.get_nsCString());
          return;
        }
        AutoJSAPI jsapi;
        if (!jsapi.Init(global)) {
          promise->MaybeRejectWithUnknownError("Could not read the tensor");
          return;
        }
        JSContext* cx = jsapi.cx();
        ErrorResult rv;
        JS::Rooted<JSObject*> buffer(
            cx, ArrayBuffer::Create(cx, aResult.get_BigBuffer().AsSpan(), rv));
        if (rv.MaybeSetPendingException(cx)) {
          promise->MaybeRejectWithUnknownError("Could not read the tensor");
          return;
        }
        promise->MaybeResolve(buffer);
      },
      [promise](mozilla::ipc::ResponseRejectReason) {
        promise->MaybeRejectWithInvalidStateError("The context is lost");
      });
  return promise.forget();
}

already_AddRefed<Promise> MLContext::ReadTensor(
    MLTensor& aTensor,
    const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aOutputData,
    ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  promise->MaybeRejectWithNotSupportedError(
      "Reading into a buffer is not supported yet");
  return promise.forget();
}

void MLContext::WriteTensor(
    MLTensor& aTensor,
    const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aInputData,
    ErrorResult& aRv) {
  if (IsLost()) {
    aRv.ThrowInvalidStateError("The context is lost");
    return;
  }
  if (aTensor.Context() != this || aTensor.IsDestroyed()) {
    aRv.ThrowTypeError("Invalid tensor");
    return;
  }
  if (!aTensor.Writable()) {
    aRv.ThrowTypeError("The tensor is not writable");
    return;
  }
  if (aInputData.IsArrayBufferView() &&
      !webnn::IsViewTypeCompatible(aInputData.GetAsArrayBufferView().Type(),
                                   aTensor.DataType())) {
    aRv.ThrowTypeError("The buffer view type does not match the data type");
    return;
  }

  Maybe<mozilla::ipc::BigBuffer> data;
  ProcessTypedArraysFixed(aInputData, [&](const Span<uint8_t>& aData) {
    if (aData.Length() == aTensor.ByteLength()) {
      data.emplace(Span<const uint8_t>(aData));
    }
  });
  if (!data) {
    aRv.ThrowTypeError("The data size does not match the tensor");
    return;
  }
  mActor->SendWriteTensor(aTensor.Id(), std::move(*data));
}

already_AddRefed<webgpu::Buffer> MLContext::ExportToGPU(MLTensor& aTensor,
                                                        ErrorResult& aRv) {
  aRv.ThrowNotSupportedError("WebGPU interop is not supported");
  return nullptr;
}

void MLContext::OpSupportLimits(MLOpSupportLimits& aRetVal) {
  nsTArray<MLOperandDataType> allTypes = {
      MLOperandDataType::Float32, MLOperandDataType::Float16,
      MLOperandDataType::Int32,   MLOperandDataType::Uint32,
      MLOperandDataType::Int64,   MLOperandDataType::Uint64,
      MLOperandDataType::Int8,    MLOperandDataType::Uint8};
  nsTArray<MLOperandDataType> floatTypes = {MLOperandDataType::Float32,
                                            MLOperandDataType::Float16};

  auto limits = [](const nsTArray<MLOperandDataType>& aTypes, uint32_t aMin,
                   uint32_t aMax) {
    MLTensorLimits limits;
    limits.mDataTypes.Construct();
    (void)limits.mDataTypes.Value().AppendElements(aTypes, fallible);
    limits.mRankRange.Construct();
    limits.mRankRange.Value().mMin.Construct(aMin);
    limits.mRankRange.Value().mMax.Construct(aMax);
    return limits;
  };
  auto binary = [&](const nsTArray<MLOperandDataType>& aTypes, uint32_t aMin) {
    MLBinarySupportLimits binary;
    binary.mA.Construct(limits(aTypes, aMin, 8));
    binary.mB.Construct(limits(aTypes, aMin, 8));
    binary.mOutput.Construct(limits(aTypes, aMin, 8));
    return binary;
  };

  aRetVal.mPreferredInputLayout.Construct(MLInputOperandLayout::Nchw);
  aRetVal.mMaxTensorByteLength.Construct(kMaxTensorByteLength);
  aRetVal.mInput.Construct(limits(allTypes, 0, 8));
  aRetVal.mConstant.Construct(limits(allTypes, 0, 8));
  aRetVal.mOutput.Construct(limits(allTypes, 0, 8));
  aRetVal.mAdd.Construct(binary(allTypes, 0));
  aRetVal.mMatmul.Construct(binary(floatTypes, 2));
  MLSingleInputSupportLimits relu;
  relu.mInput.Construct(limits(floatTypes, 0, 8));
  relu.mOutput.Construct(limits(floatTypes, 0, 8));
  aRetVal.mRelu.Construct(std::move(relu));
}

}  // namespace mozilla::dom
