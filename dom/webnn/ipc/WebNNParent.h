/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_IPC_WEBNNPARENT_H_
#define DOM_WEBNN_IPC_WEBNNPARENT_H_

#include "mozilla/UniquePtr.h"
#include "mozilla/dom/ipc/IdType.h"
#include "mozilla/webnn/PWebNNParent.h"
#include "nsTHashMap.h"

#ifdef XP_MACOSX
#  include "mozilla/webnn/rustnn.h"
#endif

namespace mozilla::webnn {

#ifdef XP_MACOSX
struct RustnnContextDeleter {
  void operator()(RustnnContext* aContext) const;
};
struct RustnnTensorDeleter {
  void operator()(RustnnTensor* aTensor) const;
};
struct RustnnGraphDeleter {
  void operator()(RustnnGraph* aGraph) const;
};
#endif

// HWInference process side of PWebNN. Everything received from the content
// process is validated again here before reaching the engine.
class WebNNParent final : public PWebNNParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(WebNNParent, override)

  explicit WebNNParent(dom::ContentParentId aContentId);

  mozilla::ipc::IPCResult RecvCreateContext(const WebNNContextOptions& aOptions,
                                            CreateContextResolver&& aResolver);
  mozilla::ipc::IPCResult RecvCreateTensor(const uint32_t& aTensorId,
                                           const WebNNOperandDesc& aDescriptor,
                                           const bool& aReadable,
                                           const bool& aWritable,
                                           CreateTensorResolver&& aResolver);
  mozilla::ipc::IPCResult RecvWriteTensor(const uint32_t& aTensorId,
                                          ipc::BigBuffer&& aData);
  mozilla::ipc::IPCResult RecvReadTensor(const uint32_t& aTensorId,
                                         ReadTensorResolver&& aResolver);
  mozilla::ipc::IPCResult RecvDestroyTensor(const uint32_t& aTensorId);
  mozilla::ipc::IPCResult RecvBuild(const uint32_t& aGraphId,
                                    const WebNNGraphDesc& aGraph,
                                    nsTArray<ipc::BigBuffer>&& aConstants,
                                    BuildResolver&& aResolver);
  mozilla::ipc::IPCResult RecvDestroyGraph(const uint32_t& aGraphId);
  mozilla::ipc::IPCResult RecvDispatch(const uint32_t& aGraphId,
                                       nsTArray<WebNNNamedTensor>&& aInputs,
                                       nsTArray<WebNNNamedTensor>&& aOutputs);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  ~WebNNParent();

  void ReleaseEngineObjects();
  void LoseContext(const nsACString& aMessage);

  const dom::ContentParentId mContentId;

#ifdef XP_MACOSX
  struct Tensor {
    mozilla::UniquePtr<RustnnTensor, RustnnTensorDeleter> mTensor;
    size_t mByteLength;
    bool mReadable;
    bool mWritable;
  };

  Tensor* GetTensor(uint32_t aTensorId);

  mozilla::UniquePtr<RustnnContext, RustnnContextDeleter> mContext;
  nsTHashMap<uint32_t, mozilla::UniquePtr<Tensor>> mTensors;
  nsTHashMap<uint32_t, mozilla::UniquePtr<RustnnGraph, RustnnGraphDeleter>>
      mGraphs;
#endif
};

}  // namespace mozilla::webnn

#endif  // DOM_WEBNN_IPC_WEBNNPARENT_H_
