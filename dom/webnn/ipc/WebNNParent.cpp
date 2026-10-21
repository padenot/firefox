/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/webnn/WebNNParent.h"

#include "mozilla/Logging.h"
#include "mozilla/webnn/WebNNIPCUtils.h"
#include "mozilla/webnn/WebNNUtils.h"

#ifdef XP_MACOSX
#  include <errno.h>
#  include <limits.h>
#  include <stdlib.h>
#  include <sys/stat.h>
#  include <unistd.h>

#  include "mozilla/llama/LlamaRuntimeLinker.h"
#endif

namespace mozilla::webnn {

static LazyLogModule gWebNNLog("WebNN");
#define LOG(fmt, ...) \
  MOZ_LOG_FMT(gWebNNLog, LogLevel::Debug, fmt, ##__VA_ARGS__)

static const char kNotSupported[] = "WebNN is not supported on this platform";

WebNNParent::WebNNParent(dom::ContentParentId aContentId)
    : mContentId(aContentId) {}

WebNNParent::~WebNNParent() { ReleaseEngineObjects(); }

void WebNNParent::ActorDestroy(ActorDestroyReason aReason) {
  ReleaseEngineObjects();
}

void WebNNParent::LoseContext(const nsACString& aMessage) {
  LOG("Losing context for content {}: {}", static_cast<uint64_t>(mContentId),
      aMessage);
  ReleaseEngineObjects();
  if (CanSend()) {
    (void)SendContextLost(nsCString(aMessage));
  }
}

#ifndef XP_MACOSX

void WebNNParent::ReleaseEngineObjects() {}

mozilla::ipc::IPCResult WebNNParent::RecvCreateContext(
    const WebNNContextOptions& aOptions, CreateContextResolver&& aResolver) {
  aResolver(nsLiteralCString(kNotSupported));
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvCreateTensor(const uint32_t&,
                                                      const WebNNOperandDesc&,
                                                      const bool&, const bool&,
                                                      CreateTensorResolver&&) {
  return IPC_FAIL(this, "No context");
}

mozilla::ipc::IPCResult WebNNParent::RecvWriteTensor(const uint32_t&,
                                                     ipc::BigBuffer&&) {
  return IPC_FAIL(this, "No context");
}

mozilla::ipc::IPCResult WebNNParent::RecvReadTensor(const uint32_t&,
                                                    ReadTensorResolver&&) {
  return IPC_FAIL(this, "No context");
}

mozilla::ipc::IPCResult WebNNParent::RecvDestroyTensor(const uint32_t&) {
  return IPC_FAIL(this, "No context");
}

mozilla::ipc::IPCResult WebNNParent::RecvBuild(const uint32_t&,
                                               const WebNNGraphDesc&,
                                               nsTArray<ipc::BigBuffer>&&,
                                               BuildResolver&&) {
  return IPC_FAIL(this, "No context");
}

mozilla::ipc::IPCResult WebNNParent::RecvDestroyGraph(const uint32_t&) {
  return IPC_FAIL(this, "No context");
}

mozilla::ipc::IPCResult WebNNParent::RecvDispatch(
    const uint32_t&, nsTArray<WebNNNamedTensor>&&,
    nsTArray<WebNNNamedTensor>&&) {
  return IPC_FAIL(this, "No context");
}

#else

static llama::LlamaLibWrapper* Lib() {
  return llama::LlamaRuntimeLinker::Get();
}

void RustnnContextDeleter::operator()(RustnnContext* aContext) const {
  Lib()->rustnn_context_destroy(aContext);
}

void RustnnTensorDeleter::operator()(RustnnTensor* aTensor) const {
  Lib()->rustnn_tensor_destroy(aTensor);
}

void RustnnGraphDeleter::operator()(RustnnGraph* aGraph) const {
  Lib()->rustnn_graph_destroy(aGraph);
}

namespace {

struct OperandDescriptorDeleter {
  void operator()(RustnnOperandDescriptor* aDescriptor) const {
    Lib()->rustnn_operand_descriptor_destroy(aDescriptor);
  }
};

struct TensorDescriptorDeleter {
  void operator()(RustnnTensorDescriptor* aDescriptor) const {
    Lib()->rustnn_tensor_descriptor_destroy(aDescriptor);
  }
};

struct OperandDeleter {
  void operator()(RustnnOperand* aOperand) const {
    Lib()->rustnn_operand_destroy(aOperand);
  }
};

struct GraphBuilderDeleter {
  void operator()(RustnnGraphBuilder* aBuilder) const {
    Lib()->rustnn_graph_builder_destroy(aBuilder);
  }
};

}  // namespace

static nsCString LastError() {
  const char* message = Lib()->rustnn_last_error_message();
  return message && *message ? nsCString(message) : "Unknown error"_ns;
}

static RustnnDataType ToRustnn(dom::MLOperandDataType aType) {
  switch (aType) {
    case dom::MLOperandDataType::Float32:
      return RustnnDataType_Float32;
    case dom::MLOperandDataType::Float16:
      return RustnnDataType_Float16;
    case dom::MLOperandDataType::Int32:
      return RustnnDataType_Int32;
    case dom::MLOperandDataType::Uint32:
      return RustnnDataType_Uint32;
    case dom::MLOperandDataType::Int64:
      return RustnnDataType_Int64;
    case dom::MLOperandDataType::Uint64:
      return RustnnDataType_Uint64;
    case dom::MLOperandDataType::Int8:
      return RustnnDataType_Int8;
    case dom::MLOperandDataType::Uint8:
      return RustnnDataType_Uint8;
  }
  MOZ_CRASH("Unexpected MLOperandDataType");
}

static RustnnPowerPreference ToRustnn(dom::MLPowerPreference aPreference) {
  switch (aPreference) {
    case dom::MLPowerPreference::Default:
      return RustnnPowerPreference_Default;
    case dom::MLPowerPreference::High_performance:
      return RustnnPowerPreference_HighPerformance;
    case dom::MLPowerPreference::Low_power:
      return RustnnPowerPreference_LowPower;
  }
  MOZ_CRASH("Unexpected MLPowerPreference");
}

static Maybe<size_t> ByteLength(const WebNNOperandDesc& aDesc) {
  return webnn::ByteLength(aDesc.dataType(), aDesc.shape());
}

static nsTArray<uint64_t> Shape(const WebNNOperandDesc& aDesc) {
  nsTArray<uint64_t> shape(aDesc.shape().Length());
  for (uint32_t dim : aDesc.shape()) {
    shape.AppendElement(dim);
  }
  return shape;
}

WebNNParent::Tensor* WebNNParent::GetTensor(uint32_t aTensorId) {
  auto* tensor = mTensors.Lookup(aTensorId).DataPtrOrNull();
  return tensor ? tensor->get() : nullptr;
}

// Points rustnn at a directory the sandbox lets this process write to. Must
// match the directory allowed by the HWInference sandbox policy.
static bool EnsureTempDirectory() {
  static bool sDone = false;
  if (sDone) {
    return true;
  }
  char tempDir[PATH_MAX];
  char realTempDir[PATH_MAX];
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, tempDir, sizeof(tempDir)) ||
      !realpath(tempDir, realTempDir)) {
    return false;
  }
  nsAutoCString dir(realTempDir);
  dir.AppendLiteral("/org.mozilla.webnn");
  if (mkdir(dir.get(), 0700) && errno != EEXIST) {
    return false;
  }
  if (Lib()->rustnn_set_temp_directory(dir.get()) != RustnnStatus_Success) {
    return false;
  }
  sDone = true;
  return true;
}

void WebNNParent::ReleaseEngineObjects() {
  mGraphs.Clear();
  mTensors.Clear();
  mContext = nullptr;
}

mozilla::ipc::IPCResult WebNNParent::RecvCreateContext(
    const WebNNContextOptions& aOptions, CreateContextResolver&& aResolver) {
  if (mContext) {
    return IPC_FAIL(this, "Context already created");
  }
  if (!llama::LlamaRuntimeLinker::Init()) {
    aResolver("The WebNN engine could not be loaded"_ns);
    return IPC_OK();
  }
  if (!EnsureTempDirectory()) {
    aResolver("Could not create the WebNN temporary directory"_ns);
    return IPC_OK();
  }

  RustnnContextOptions options = Lib()->rustnn_context_options_default();
  options.power_preference = ToRustnn(aOptions.powerPreference());
  options.accelerated = aOptions.accelerated();

  RustnnContext* context = nullptr;
  if (Lib()->rustnn_context_create(&options, &context) !=
      RustnnStatus_Success) {
    aResolver(LastError());
    return IPC_OK();
  }
  mContext.reset(context);
  LOG("Created context for content {}", static_cast<uint64_t>(mContentId));
  aResolver(""_ns);
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvCreateTensor(
    const uint32_t& aTensorId, const WebNNOperandDesc& aDescriptor,
    const bool& aReadable, const bool& aWritable,
    CreateTensorResolver&& aResolver) {
  if (!mContext) {
    aResolver("Context lost"_ns);
    return IPC_OK();
  }
  if (mTensors.Contains(aTensorId)) {
    return IPC_FAIL(this, "Duplicate tensor id");
  }
  Maybe<size_t> byteLength = ByteLength(aDescriptor);
  if (!byteLength) {
    return IPC_FAIL(this, "Tensor too large");
  }

  nsTArray<uint64_t> shape = Shape(aDescriptor);
  RustnnTensorDescriptor* rawDescriptor = nullptr;
  if (Lib()->rustnn_tensor_descriptor_create(
          ToRustnn(aDescriptor.dataType()), shape.Elements(), shape.Length(),
          aReadable, aWritable, &rawDescriptor) != RustnnStatus_Success) {
    aResolver(LastError());
    return IPC_OK();
  }
  mozilla::UniquePtr<RustnnTensorDescriptor, TensorDescriptorDeleter>
      descriptor(rawDescriptor);

  RustnnTensor* tensor = nullptr;
  if (Lib()->rustnn_context_create_tensor(mContext.get(), descriptor.get(),
                                          &tensor) != RustnnStatus_Success) {
    aResolver(LastError());
    return IPC_OK();
  }

  auto entry = MakeUnique<Tensor>();
  entry->mTensor.reset(tensor);
  entry->mByteLength = *byteLength;
  entry->mReadable = aReadable;
  entry->mWritable = aWritable;
  mTensors.InsertOrUpdate(aTensorId, std::move(entry));
  aResolver(""_ns);
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvWriteTensor(const uint32_t& aTensorId,
                                                     ipc::BigBuffer&& aData) {
  if (!mContext) {
    return IPC_OK();
  }
  Tensor* tensor = GetTensor(aTensorId);
  if (!tensor) {
    return IPC_FAIL(this, "Unknown tensor");
  }
  if (!tensor->mWritable || aData.Size() != tensor->mByteLength) {
    return IPC_FAIL(this, "Invalid tensor write");
  }
  if (Lib()->rustnn_context_write_tensor(mContext.get(), tensor->mTensor.get(),
                                         aData.Data(), aData.Size()) !=
      RustnnStatus_Success) {
    LoseContext(LastError());
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvReadTensor(
    const uint32_t& aTensorId, ReadTensorResolver&& aResolver) {
  if (!mContext) {
    aResolver(WebNNReadTensorResult("Context lost"_ns));
    return IPC_OK();
  }
  Tensor* tensor = GetTensor(aTensorId);
  if (!tensor) {
    return IPC_FAIL(this, "Unknown tensor");
  }
  if (!tensor->mReadable) {
    return IPC_FAIL(this, "Tensor is not readable");
  }

  ipc::BigBuffer data(tensor->mByteLength);
  if (Lib()->rustnn_context_read_tensor(mContext.get(), tensor->mTensor.get(),
                                        data.Data(),
                                        data.Size()) != RustnnStatus_Success) {
    aResolver(WebNNReadTensorResult(LastError()));
    return IPC_OK();
  }
  aResolver(std::move(data));
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvDestroyTensor(
    const uint32_t& aTensorId) {
  mTensors.Remove(aTensorId);
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvBuild(
    const uint32_t& aGraphId, const WebNNGraphDesc& aGraph,
    nsTArray<ipc::BigBuffer>&& aConstants, BuildResolver&& aResolver) {
  if (!mContext) {
    aResolver("Context lost"_ns);
    return IPC_OK();
  }
  if (mGraphs.Contains(aGraphId)) {
    return IPC_FAIL(this, "Duplicate graph id");
  }

  const nsTArray<WebNNOperandDesc>& descs = aGraph.operands();
  nsTArray<mozilla::UniquePtr<RustnnOperand, OperandDeleter> > operands;
  operands.SetLength(descs.Length());
  auto defined = [&](uint32_t aIndex) {
    return aIndex < operands.Length() && operands[aIndex];
  };
  auto definable = [&](uint32_t aIndex) {
    return aIndex < operands.Length() && !operands[aIndex];
  };

  RustnnGraphBuilder* rawBuilder = nullptr;
  if (Lib()->rustnn_graph_builder_create(mContext.get(), &rawBuilder) !=
      RustnnStatus_Success) {
    aResolver(LastError());
    return IPC_OK();
  }
  RustnnGraphBuilder* builder = rawBuilder;
  mozilla::UniquePtr<RustnnGraphBuilder, GraphBuilderDeleter> builderOwner(
      rawBuilder);

  for (const WebNNOperation& op : aGraph.operations()) {
    RustnnOperand* result = nullptr;
    RustnnStatus status = RustnnStatus_Success;
    uint32_t output;

    switch (op.type()) {
      case WebNNOperation::TWebNNInputOp: {
        const WebNNInputOp& input = op.get_WebNNInputOp();
        output = input.output();
        if (!definable(output)) {
          return IPC_FAIL(this, "Invalid input operand");
        }
        nsTArray<uint64_t> shape = Shape(descs[output]);
        RustnnOperandDescriptor* rawDescriptor = nullptr;
        status = Lib()->rustnn_operand_descriptor_create(
            ToRustnn(descs[output].dataType()), shape.Elements(),
            shape.Length(), &rawDescriptor);
        if (status != RustnnStatus_Success) {
          break;
        }
        mozilla::UniquePtr<RustnnOperandDescriptor, OperandDescriptorDeleter>
            descriptor(rawDescriptor);
        status = Lib()->rustnn_graph_builder_input(builder, input.name().get(),
                                                   descriptor.get(), &result);
        break;
      }
      case WebNNOperation::TWebNNConstantOp: {
        const WebNNConstantOp& constant = op.get_WebNNConstantOp();
        output = constant.output();
        if (!definable(output) ||
            constant.constantIndex() >= aConstants.Length()) {
          return IPC_FAIL(this, "Invalid constant operand");
        }
        const ipc::BigBuffer& data = aConstants[constant.constantIndex()];
        Maybe<size_t> byteLength = ByteLength(descs[output]);
        if (!byteLength || *byteLength != data.Size()) {
          return IPC_FAIL(this, "Invalid constant size");
        }
        nsTArray<uint64_t> shape = Shape(descs[output]);
        RustnnOperandDescriptor* rawDescriptor = nullptr;
        status = Lib()->rustnn_operand_descriptor_create(
            ToRustnn(descs[output].dataType()), shape.Elements(),
            shape.Length(), &rawDescriptor);
        if (status != RustnnStatus_Success) {
          break;
        }
        mozilla::UniquePtr<RustnnOperandDescriptor, OperandDescriptorDeleter>
            descriptor(rawDescriptor);
        status = Lib()->rustnn_graph_builder_constant(
            builder, descriptor.get(), data.Data(), data.Size(), &result);
        break;
      }
      case WebNNOperation::TWebNNAddOp: {
        const WebNNAddOp& add = op.get_WebNNAddOp();
        output = add.output();
        if (!defined(add.a()) || !defined(add.b()) || !definable(output)) {
          return IPC_FAIL(this, "Invalid add operands");
        }
        status = Lib()->rustnn_graph_builder_add(
            builder, operands[add.a()].get(), operands[add.b()].get(), &result);
        break;
      }
      case WebNNOperation::TWebNNMatmulOp: {
        const WebNNMatmulOp& matmul = op.get_WebNNMatmulOp();
        output = matmul.output();
        if (!defined(matmul.a()) || !defined(matmul.b()) ||
            !definable(output)) {
          return IPC_FAIL(this, "Invalid matmul operands");
        }
        status = Lib()->rustnn_graph_builder_matmul(
            builder, operands[matmul.a()].get(), operands[matmul.b()].get(),
            &result);
        break;
      }
      case WebNNOperation::TWebNNReluOp: {
        const WebNNReluOp& relu = op.get_WebNNReluOp();
        output = relu.output();
        if (!defined(relu.input()) || !definable(output)) {
          return IPC_FAIL(this, "Invalid relu operands");
        }
        status = Lib()->rustnn_graph_builder_relu(
            builder, operands[relu.input()].get(), &result);
        break;
      }
      default:
        return IPC_FAIL(this, "Unknown operation");
    }

    if (status != RustnnStatus_Success) {
      aResolver(LastError());
      return IPC_OK();
    }
    operands[output].reset(result);
  }

  nsTArray<RustnnNamedOperand> outputs(aGraph.outputs().Length());
  for (const WebNNNamedOperand& output : aGraph.outputs()) {
    if (!defined(output.operand())) {
      return IPC_FAIL(this, "Invalid graph output");
    }
    outputs.AppendElement(RustnnNamedOperand{output.name().get(),
                                             operands[output.operand()].get()});
  }

  RustnnGraph* graph = nullptr;
  RustnnStatus status = Lib()->rustnn_graph_builder_build(
      &builder, outputs.Elements(), outputs.Length(), &graph);
  // build() consumes the builder, and nulls out the pointer it was given.
  if (!builder) {
    (void)builderOwner.release();
  }
  if (status != RustnnStatus_Success) {
    aResolver(LastError());
    return IPC_OK();
  }
  mGraphs.InsertOrUpdate(
      aGraphId, mozilla::UniquePtr<RustnnGraph, RustnnGraphDeleter>(graph));
  aResolver(""_ns);
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvDestroyGraph(
    const uint32_t& aGraphId) {
  mGraphs.Remove(aGraphId);
  return IPC_OK();
}

mozilla::ipc::IPCResult WebNNParent::RecvDispatch(
    const uint32_t& aGraphId, nsTArray<WebNNNamedTensor>&& aInputs,
    nsTArray<WebNNNamedTensor>&& aOutputs) {
  if (!mContext) {
    return IPC_OK();
  }
  auto* graph = mGraphs.Lookup(aGraphId).DataPtrOrNull();
  if (!graph) {
    return IPC_FAIL(this, "Unknown graph");
  }

  auto convert = [&](const nsTArray<WebNNNamedTensor>& aNamed,
                     nsTArray<RustnnNamedTensor>& aOut) {
    for (const WebNNNamedTensor& named : aNamed) {
      Tensor* tensor = GetTensor(named.tensor());
      if (!tensor) {
        return false;
      }
      aOut.AppendElement(
          RustnnNamedTensor{named.name().get(), tensor->mTensor.get()});
    }
    return true;
  };
  nsTArray<RustnnNamedTensor> inputs(aInputs.Length());
  nsTArray<RustnnNamedTensor> outputs(aOutputs.Length());
  if (!convert(aInputs, inputs) || !convert(aOutputs, outputs)) {
    return IPC_FAIL(this, "Unknown tensor");
  }

  if (Lib()->rustnn_context_dispatch(
          mContext.get(), graph->get(), inputs.Elements(), inputs.Length(),
          outputs.Elements(), outputs.Length()) != RustnnStatus_Success) {
    LoseContext(LastError());
  }
  return IPC_OK();
}

#endif  // XP_MACOSX

}  // namespace mozilla::webnn
