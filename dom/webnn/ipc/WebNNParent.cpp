/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/webnn/WebNNParent.h"

#include "mozilla/Logging.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/webnn/WebNNIPCUtils.h"
#include "mozilla/webnn/WebNNUtils.h"

#ifdef XP_MACOSX
#  include <errno.h>
#  include <limits.h>
#  include <stdlib.h>
#  include <sys/stat.h>
#  include <unistd.h>

#  include "mozilla/llama/LlamaRuntimeLinker.h"
#  include "mozilla/webnn/WebNNParentOps.h"
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

void WebnnContextDeleter::operator()(WebnnContext* aContext) const {
  Lib()->webnn_context_destroy(aContext);
}

void WebnnTensorDeleter::operator()(WebnnTensor* aTensor) const {
  Lib()->webnn_tensor_destroy(aTensor);
}

void WebnnGraphDeleter::operator()(WebnnGraph* aGraph) const {
  Lib()->webnn_graph_destroy(aGraph);
}

namespace {

struct GraphBuilderDeleter {
  void operator()(WebnnGraphBuilder* aBuilder) const {
    Lib()->webnn_graph_builder_destroy(aBuilder);
  }
};

}  // namespace

static nsCString LastError() {
  const char* message = Lib()->webnn_last_error_message();
  return message && *message ? nsCString(message) : "Unknown error"_ns;
}

static WebnnDataType ToFfi(dom::MLOperandDataType aType) {
  switch (aType) {
    case dom::MLOperandDataType::Float32:
      return WebnnDataType_Float32;
    case dom::MLOperandDataType::Float16:
      return WebnnDataType_Float16;
    case dom::MLOperandDataType::Int32:
      return WebnnDataType_Int32;
    case dom::MLOperandDataType::Uint32:
      return WebnnDataType_Uint32;
    case dom::MLOperandDataType::Int64:
      return WebnnDataType_Int64;
    case dom::MLOperandDataType::Uint64:
      return WebnnDataType_Uint64;
    case dom::MLOperandDataType::Int8:
      return WebnnDataType_Int8;
    case dom::MLOperandDataType::Uint8:
      return WebnnDataType_Uint8;
  }
  MOZ_CRASH("Unexpected MLOperandDataType");
}

static WebnnPowerPreference ToFfi(dom::MLPowerPreference aPreference) {
  switch (aPreference) {
    case dom::MLPowerPreference::Default:
      return WebnnPowerPreference_Default;
    case dom::MLPowerPreference::High_performance:
      return WebnnPowerPreference_HighPerformance;
    case dom::MLPowerPreference::Low_power:
      return WebnnPowerPreference_LowPower;
  }
  MOZ_CRASH("Unexpected MLPowerPreference");
}

static WebnnOperandDescriptor ToFfi(const WebNNOperandDesc& aDesc) {
  return {ToFfi(aDesc.dataType()), aDesc.shape().Elements(),
          aDesc.shape().Length()};
}

static Maybe<size_t> ByteLength(const WebNNOperandDesc& aDesc) {
  return webnn::ByteLength(aDesc.dataType(), aDesc.shape());
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
  if (Lib()->webnn_set_temp_directory(dir.get()) != WebnnStatus_Success) {
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

  AUTO_PROFILER_MARKER_TEXT("WebNN:CreateContext", ML_SETUP, {}, ""_ns);
  WebnnContext* context = nullptr;
  if (Lib()->webnn_context_create(ToFfi(aOptions.powerPreference()),
                                  aOptions.accelerated(),
                                  &context) != WebnnStatus_Success) {
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

  WebnnOperandDescriptor descriptor = ToFfi(aDescriptor);
  WebnnTensor* tensor = nullptr;
  if (Lib()->webnn_context_create_tensor(mContext.get(), &descriptor, aReadable,
                                         aWritable,
                                         &tensor) != WebnnStatus_Success) {
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
  AUTO_PROFILER_MARKER_TEXT("WebNN:WriteTensor", ML_INFERENCE, {},
                            nsPrintfCString("%zu bytes", aData.Size()));
  if (Lib()->webnn_context_write_tensor(mContext.get(), tensor->mTensor.get(),
                                        aData.Data(),
                                        aData.Size()) != WebnnStatus_Success) {
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

  AUTO_PROFILER_MARKER_TEXT("WebNN:ReadTensor", ML_INFERENCE, {},
                            nsPrintfCString("%zu bytes", tensor->mByteLength));
  ipc::BigBuffer data(tensor->mByteLength);
  if (Lib()->webnn_context_read_tensor(mContext.get(), tensor->mTensor.get(),
                                       data.Data(),
                                       data.Size()) != WebnnStatus_Success) {
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
  size_t constantBytes = 0;
  for (const ipc::BigBuffer& constant : aConstants) {
    constantBytes += constant.Size();
  }
  AUTO_PROFILER_MARKER_TEXT(
      "WebNN:Build", ML_SETUP, {},
      nsPrintfCString("%zu operations, %zu constants, %zu bytes",
                      aGraph.operations().Length(), aConstants.Length(),
                      constantBytes));

  const nsTArray<WebNNOperandDesc>& descs = aGraph.operands();
  // Engine operand id of each operand, once defined.
  nsTArray<Maybe<uint32_t> > operands;
  operands.SetLength(descs.Length());
  auto definable = [&](uint32_t aIndex) {
    return aIndex < operands.Length() && !operands[aIndex];
  };

  WebnnGraphBuilder* rawBuilder = nullptr;
  if (Lib()->webnn_graph_builder_create(mContext.get(), &rawBuilder) !=
      WebnnStatus_Success) {
    aResolver(LastError());
    return IPC_OK();
  }
  mozilla::UniquePtr<WebnnGraphBuilder, GraphBuilderDeleter> builder(
      rawBuilder);

  for (const WebNNOperation& op : aGraph.operations()) {
    if (op.type() != WebNNOperation::TWebNNInputOp &&
        op.type() != WebNNOperation::TWebNNConstantOp) {
      switch (AddOperation(Lib(), builder.get(), op, operands)) {
        case AddOperationResult::Ok:
          continue;
        case AddOperationResult::EngineError:
          aResolver(LastError());
          return IPC_OK();
        case AddOperationResult::OutputMismatch:
          aResolver("Unexpected number of operator outputs"_ns);
          return IPC_OK();
        case AddOperationResult::Invalid:
          return IPC_FAIL(this, "Invalid operation");
      }
    }

    uint32_t output;
    uint32_t id;
    WebnnStatus status;
    if (op.type() == WebNNOperation::TWebNNInputOp) {
      const WebNNInputOp& input = op.get_WebNNInputOp();
      output = input.output();
      if (!definable(output)) {
        return IPC_FAIL(this, "Invalid input operand");
      }
      WebnnOperandDescriptor descriptor = ToFfi(descs[output]);
      status = Lib()->webnn_graph_builder_input(
          builder.get(), input.name().get(), &descriptor, &id);
    } else {
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
      WebnnOperandDescriptor descriptor = ToFfi(descs[output]);
      status = Lib()->webnn_graph_builder_constant(
          builder.get(), &descriptor, data.Data(), data.Size(), &id);
    }
    if (status != WebnnStatus_Success) {
      aResolver(LastError());
      return IPC_OK();
    }
    operands[output] = Some(id);
  }

  nsTArray<WebnnNamedOperand> outputs(aGraph.outputs().Length());
  for (const WebNNNamedOperand& output : aGraph.outputs()) {
    if (output.operand() >= operands.Length() || !operands[output.operand()]) {
      return IPC_FAIL(this, "Invalid graph output");
    }
    outputs.AppendElement(
        WebnnNamedOperand{output.name().get(), *operands[output.operand()]});
  }

  AUTO_PROFILER_MARKER_TEXT("WebNN:Compile", ML_SETUP, {}, ""_ns);
  WebnnGraph* graph = nullptr;
  if (Lib()->webnn_graph_builder_build(builder.get(), outputs.Elements(),
                                       outputs.Length(),
                                       &graph) != WebnnStatus_Success) {
    aResolver(LastError());
    return IPC_OK();
  }
  mGraphs.InsertOrUpdate(
      aGraphId, mozilla::UniquePtr<WebnnGraph, WebnnGraphDeleter>(graph));
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
                     nsTArray<WebnnNamedTensor>& aOut) {
    for (const WebNNNamedTensor& named : aNamed) {
      Tensor* tensor = GetTensor(named.tensor());
      if (!tensor) {
        return false;
      }
      aOut.AppendElement(
          WebnnNamedTensor{named.name().get(), tensor->mTensor.get()});
    }
    return true;
  };
  nsTArray<WebnnNamedTensor> inputs(aInputs.Length());
  nsTArray<WebnnNamedTensor> outputs(aOutputs.Length());
  if (!convert(aInputs, inputs) || !convert(aOutputs, outputs)) {
    return IPC_FAIL(this, "Unknown tensor");
  }

  AUTO_PROFILER_MARKER_TEXT("WebNN:Dispatch", ML_INFERENCE, {},
                            nsPrintfCString("graph %u", aGraphId));
  if (Lib()->webnn_context_dispatch(
          mContext.get(), graph->get(), inputs.Elements(), inputs.Length(),
          outputs.Elements(), outputs.Length()) != WebnnStatus_Success) {
    LoseContext(LastError());
  }
  return IPC_OK();
}

#endif  // XP_MACOSX

}  // namespace mozilla::webnn
