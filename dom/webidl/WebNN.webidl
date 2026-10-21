/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://webmachinelearning.github.io/webnn/
 *
 * Only a subset of the MLGraphBuilder operators is present for now.
 * FrozenArray is spelled [Frozen, Cached, Pure] sequence, and MLNumber is
 * unrestricted double because bigint is not supported by our bindings.
 */

interface mixin NavigatorML {
  [SecureContext, SameObject, Pref="dom.webnn.enabled"] readonly attribute ML ml;
};

enum MLPowerPreference {
  "default",
  "high-performance",
  "low-power"
};

dictionary MLContextOptions {
  MLPowerPreference powerPreference = "default";
  boolean accelerated = true;
};

[SecureContext, Exposed=Window, Pref="dom.webnn.enabled"]
interface ML {
  [NewObject]
  Promise<MLContext> createContext(optional MLContextOptions options = {});
  [NewObject]
  Promise<MLContext> createContext(GPUDevice gpuDevice);
};

typedef record<USVString, MLTensor> MLNamedTensors;

dictionary MLContextLostInfo {
  DOMString message;
};

[SecureContext, Exposed=Window, Pref="dom.webnn.enabled"]
interface MLContext {
  [Throws]
  undefined dispatch(MLGraph graph, MLNamedTensors inputs, MLNamedTensors outputs);

  [NewObject]
  Promise<MLTensor> createTensor(MLTensorDescriptor descriptor);
  [NewObject]
  Promise<MLTensor> createExportableTensor(
    MLTensorDescriptor descriptor, GPUDevice gpuDevice);
  [NewObject]
  Promise<MLTensor> createConstantTensor(
    MLOperandDescriptor descriptor, AllowSharedBufferSource inputData);

  [NewObject]
  Promise<ArrayBuffer> readTensor(MLTensor tensor);
  [NewObject]
  Promise<undefined> readTensor(MLTensor tensor, AllowSharedBufferSource outputData);

  [Throws]
  undefined writeTensor(MLTensor tensor, AllowSharedBufferSource inputData);

  [Throws]
  GPUBuffer exportToGPU(MLTensor tensor);

  MLOpSupportLimits opSupportLimits();

  undefined destroy();

  readonly attribute boolean accelerated;
  readonly attribute Promise<MLContextLostInfo> lost;
};

dictionary MLOpSupportLimits {
  MLInputOperandLayout preferredInputLayout;
  [EnforceRange] unsigned long long maxTensorByteLength;
  MLTensorLimits input;
  MLTensorLimits constant;
  MLTensorLimits output;
};

dictionary MLRankRange {
  unsigned long min;
  unsigned long max;
};

typedef sequence<MLOperandDataType> MLDataTypeList;

dictionary MLTensorLimits {
  MLDataTypeList dataTypes;
  MLRankRange rankRange;
};

dictionary MLBinarySupportLimits {
  MLTensorLimits a;
  MLTensorLimits b;
  MLTensorLimits output;
};

dictionary MLSingleInputSupportLimits {
  MLTensorLimits input;
  MLTensorLimits output;
};

[SecureContext, Exposed=Window, Pref="dom.webnn.enabled"]
interface MLGraph {
  undefined destroy();
};

enum MLInputOperandLayout {
  "nchw",
  "nhwc"
};

enum MLOperandDataType {
  "float32",
  "float16",
  "int32",
  "uint32",
  "int64",
  "uint64",
  "int8",
  "uint8"
};

dictionary MLOperandDescriptor {
  required MLOperandDataType dataType;
  required sequence<[EnforceRange] unsigned long> shape;
};

[SecureContext, Exposed=Window, Pref="dom.webnn.enabled"]
interface MLOperand {
  readonly attribute MLOperandDataType dataType;
  [Frozen, Cached, Pure]
  readonly attribute sequence<unsigned long> shape;
};

dictionary MLOperatorOptions {
  USVString label = "";
};

typedef unrestricted double MLNumber;

dictionary MLTensorDescriptor : MLOperandDescriptor {
  boolean readable = false;
  boolean writable = false;
};

[SecureContext, Exposed=Window, Pref="dom.webnn.enabled"]
interface MLTensor {
  readonly attribute MLOperandDataType dataType;
  [Frozen, Cached, Pure]
  readonly attribute sequence<unsigned long> shape;
  readonly attribute boolean readable;
  readonly attribute boolean writable;
  readonly attribute boolean constant;

  undefined destroy();
};

typedef record<USVString, MLOperand> MLNamedOperands;

[SecureContext, Exposed=Window, Pref="dom.webnn.enabled"]
interface MLGraphBuilder {
  [Throws]
  constructor(MLContext context);

  [Throws]
  MLOperand input(USVString name, MLOperandDescriptor descriptor);

  [Throws]
  MLOperand constant(MLOperandDescriptor descriptor,
                     AllowSharedBufferSource buffer);

  [Throws]
  MLOperand constant(MLOperandDataType dataType, MLNumber value);

  [Throws]
  MLOperand constant(MLTensor tensor);

  [NewObject]
  Promise<MLGraph> build(MLNamedOperands outputs);
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand add(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLBinarySupportLimits add;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand matmul(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLBinarySupportLimits matmul;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand relu(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits relu;
};
