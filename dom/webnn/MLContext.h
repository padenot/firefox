/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_MLCONTEXT_H_
#define DOM_WEBNN_MLCONTEXT_H_

#include "mozilla/dom/BufferSourceBinding.h"
#include "mozilla/dom/Record.h"
#include "mozilla/dom/TypedArray.h"
#include "mozilla/dom/WebNNBinding.h"
#include "nsIGlobalObject.h"
#include "nsWrapperCache.h"

namespace mozilla::webnn {
class WebNNChild;
}

namespace mozilla::webgpu {
class Buffer;
class Device;
}  // namespace mozilla::webgpu

namespace mozilla::dom {

class MLGraph;
class MLTensor;
class Promise;

class MLContext final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MLContext)

  static constexpr uint64_t kMaxTensorByteLength = INT32_MAX;

  // Implements ML.createContext(): resolves aPromise with a new context once
  // the HWInference process has created it.
  static void Create(nsIGlobalObject* aGlobal, const MLContextOptions& aOptions,
                     Promise* aPromise);

  nsIGlobalObject* GetParentObject() const { return mGlobal; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  void Dispatch(MLGraph& aGraph,
                const Record<nsString, OwningNonNull<MLTensor>>& aInputs,
                const Record<nsString, OwningNonNull<MLTensor>>& aOutputs,
                ErrorResult& aRv);
  already_AddRefed<Promise> CreateTensor(const MLTensorDescriptor& aDescriptor,
                                         ErrorResult& aRv);
  already_AddRefed<Promise> CreateExportableTensor(
      const MLTensorDescriptor& aDescriptor, webgpu::Device& aDevice,
      ErrorResult& aRv);
  already_AddRefed<Promise> CreateConstantTensor(
      const MLOperandDescriptor& aDescriptor,
      const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aInputData,
      ErrorResult& aRv);
  already_AddRefed<Promise> ReadTensor(MLTensor& aTensor, ErrorResult& aRv);
  already_AddRefed<Promise> ReadTensor(
      MLTensor& aTensor,
      const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aOutputData,
      ErrorResult& aRv);
  void WriteTensor(
      MLTensor& aTensor,
      const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView& aInputData,
      ErrorResult& aRv);
  already_AddRefed<webgpu::Buffer> ExportToGPU(MLTensor& aTensor,
                                               ErrorResult& aRv);
  void OpSupportLimits(MLOpSupportLimits& aRetVal);
  void Destroy();
  bool Accelerated() const { return mAccelerated; }
  Promise* Lost() const { return mLostPromise; }

  webnn::WebNNChild* Actor() const { return mActor; }
  bool IsLost() const { return !mActor; }
  uint32_t NextGraphId() { return mNextGraphId++; }

  void OnContextLost(const nsACString& aMessage);

 private:
  already_AddRefed<Promise> ReadTensorInternal(
      MLTensor& aTensor,
      const MaybeSharedArrayBufferOrMaybeSharedArrayBufferView* aOutputData,
      ErrorResult& aRv);

  MLContext(nsIGlobalObject* aGlobal, bool aAccelerated, Promise* aLostPromise);
  ~MLContext();

  nsCOMPtr<nsIGlobalObject> mGlobal;
  RefPtr<webnn::WebNNChild> mActor;
  RefPtr<Promise> mLostPromise;
  const bool mAccelerated;
  uint32_t mNextTensorId = 0;
  uint32_t mNextGraphId = 0;
};

}  // namespace mozilla::dom

#endif  // DOM_WEBNN_MLCONTEXT_H_
