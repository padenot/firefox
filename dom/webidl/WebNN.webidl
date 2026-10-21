/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://webmachinelearning.github.io/webnn/
 *
 * Copied from testing/web-platform/tests/interfaces/webnn.idl, with these
 * deviations: Window only and behind dom.webnn.enabled, FrozenArray is spelled
 * [Frozen, Cached, Pure] sequence, and MLNumber is unrestricted double because
 * bigint is not supported by our bindings. The MLGraphBuilder operator plumbing
 * is generated from this file, see dom/webnn/codegen.
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
  // Construct the graph builder from the context.
  [Throws]
  constructor(MLContext context);

  // Create an operand for a graph input.
  [Throws]
  MLOperand input(USVString name, MLOperandDescriptor descriptor);

  // Create an operand for a graph constant.
  [Throws]
  MLOperand constant(MLOperandDescriptor descriptor,
                     AllowSharedBufferSource buffer);

  // Create a scalar operand from the specified number of the specified type.
  [Throws]
  MLOperand constant(MLOperandDataType dataType, MLNumber value);

  // Create an operand from a specified constant tensor.
  [Throws]
  MLOperand constant(MLTensor tensor);

  // Compile the graph up to the specified output operands asynchronously.
  [NewObject]
  Promise<MLGraph> build(MLNamedOperands outputs);
};

dictionary MLArgMinMaxOptions : MLOperatorOptions {
  boolean keepDimensions = false;
  MLOperandDataType outputDataType = "int32";
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand argMin(MLOperand input, [EnforceRange] unsigned long axis,
                   optional MLArgMinMaxOptions options = {});
  [Throws]
  MLOperand argMax(MLOperand input, [EnforceRange] unsigned long axis,
                   optional MLArgMinMaxOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits argMin;
  MLSingleInputSupportLimits argMax;
};

dictionary MLBatchNormalizationOptions : MLOperatorOptions {
  MLOperand scale;
  MLOperand bias;
  [EnforceRange] unsigned long axis = 1;
  double epsilon = 1e-5;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand batchNormalization(MLOperand input, MLOperand mean, MLOperand variance,
                               optional MLBatchNormalizationOptions options = {});
};

dictionary MLBatchNormalizationSupportLimits {
  MLTensorLimits input;
  MLTensorLimits mean;
  MLTensorLimits variance;
  MLTensorLimits scale;
  MLTensorLimits bias;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLBatchNormalizationSupportLimits batchNormalization;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand cast(MLOperand input,
                 MLOperandDataType dataType,
                 optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits cast;
};

dictionary MLClampOptions : MLOperatorOptions {
  MLNumber minValue;
  MLNumber maxValue;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand clamp(MLOperand input, optional MLClampOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits clamp;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand concat(sequence<MLOperand> inputs,
                   [EnforceRange] unsigned long axis,
                   optional MLOperatorOptions options = {});
};

dictionary MLConcatSupportLimits {
  MLTensorLimits inputs;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLConcatSupportLimits concat;
};

enum MLConv2dFilterOperandLayout {
  "oihw",
  "hwio",
  "ohwi",
  "ihwo"
};

dictionary MLConv2dOptions : MLOperatorOptions {
  sequence<[EnforceRange] unsigned long> padding;
  sequence<[EnforceRange] unsigned long> strides;
  sequence<[EnforceRange] unsigned long> dilations;
  [EnforceRange] unsigned long groups = 1;
  MLInputOperandLayout inputLayout = "nchw";
  MLConv2dFilterOperandLayout filterLayout = "oihw";
  MLOperand bias;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand conv2d(MLOperand input,
                   MLOperand filter,
                   optional MLConv2dOptions options = {});
};

dictionary MLConv2dSupportLimits {
  MLTensorLimits input;
  MLTensorLimits filter;
  MLTensorLimits bias;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLConv2dSupportLimits conv2d;
};

enum MLConvTranspose2dFilterOperandLayout {
  "iohw",
  "hwoi",
  "ohwi"
};

dictionary MLConvTranspose2dOptions : MLOperatorOptions {
  sequence<[EnforceRange] unsigned long> padding;
  sequence<[EnforceRange] unsigned long> strides;
  sequence<[EnforceRange] unsigned long> dilations;
  sequence<[EnforceRange] unsigned long> outputPadding;
  sequence<[EnforceRange] unsigned long> outputSizes;
  [EnforceRange] unsigned long groups = 1;
  MLInputOperandLayout inputLayout = "nchw";
  MLConvTranspose2dFilterOperandLayout filterLayout = "iohw";
  MLOperand bias;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand convTranspose2d(MLOperand input, MLOperand filter,
                            optional MLConvTranspose2dOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLConv2dSupportLimits convTranspose2d;
};

dictionary MLCumulativeSumOptions : MLOperatorOptions {
  boolean exclusive = false;
  boolean reversed = false;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand cumulativeSum(MLOperand input,
                          unsigned long axis,
                          optional MLCumulativeSumOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits cumulativeSum;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand add(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand sub(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand mul(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand div(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand max(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand min(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand pow(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLBinarySupportLimits add;
  MLBinarySupportLimits sub;
  MLBinarySupportLimits mul;
  MLBinarySupportLimits div;
  MLBinarySupportLimits max;
  MLBinarySupportLimits min;
  MLBinarySupportLimits pow;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand equal(MLOperand a,
                  MLOperand b,
                  optional MLOperatorOptions options = {});
  [Throws]
  MLOperand notEqual(MLOperand a,
                     MLOperand b,
                     optional MLOperatorOptions options = {});
  [Throws]
  MLOperand greater(MLOperand a,
                    MLOperand b,
                    optional MLOperatorOptions options = {});
  [Throws]
  MLOperand greaterOrEqual(MLOperand a,
                           MLOperand b,
                           optional MLOperatorOptions options = {});
  [Throws]
  MLOperand lesser(MLOperand a,
                   MLOperand b,
                   optional MLOperatorOptions options = {});
  [Throws]
  MLOperand lesserOrEqual(MLOperand a,
                          MLOperand b,
                          optional MLOperatorOptions options = {});
  [Throws]
  MLOperand logicalNot(MLOperand a, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand logicalAnd(MLOperand a,
                       MLOperand b,
                       optional MLOperatorOptions options = {});
  [Throws]
  MLOperand logicalOr(MLOperand a,
                      MLOperand b,
                      optional MLOperatorOptions options = {});
  [Throws]
  MLOperand logicalXor(MLOperand a,
                       MLOperand b,
                       optional MLOperatorOptions options = {});
  [Throws]
  MLOperand isNaN(MLOperand a, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand isInfinite(MLOperand a, optional MLOperatorOptions options = {});
};

dictionary MLLogicalNotSupportLimits {
  MLTensorLimits a;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLBinarySupportLimits equal;
  MLBinarySupportLimits notEqual;
  MLBinarySupportLimits greater;
  MLBinarySupportLimits greaterOrEqual;
  MLBinarySupportLimits lesser;
  MLBinarySupportLimits lesserOrEqual;
  MLLogicalNotSupportLimits logicalNot;
  MLBinarySupportLimits logicalAnd;
  MLBinarySupportLimits logicalOr;
  MLBinarySupportLimits logicalXor;
  MLLogicalNotSupportLimits isNaN;
  MLLogicalNotSupportLimits isInfinite;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand abs(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand ceil(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand cos(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand erf(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand exp(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand floor(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand identity(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand log(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand neg(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand reciprocal(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand roundEven(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand sin(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand sign(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand sqrt(MLOperand input, optional MLOperatorOptions options = {});
  [Throws]
  MLOperand tan(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits abs;
  MLSingleInputSupportLimits ceil;
  MLSingleInputSupportLimits cos;
  MLSingleInputSupportLimits erf;
  MLSingleInputSupportLimits exp;
  MLSingleInputSupportLimits floor;
  MLSingleInputSupportLimits identity;
  MLSingleInputSupportLimits log;
  MLSingleInputSupportLimits neg;
  MLSingleInputSupportLimits reciprocal;
  MLSingleInputSupportLimits roundEven;
  MLSingleInputSupportLimits sin;
  MLSingleInputSupportLimits sign;
  MLSingleInputSupportLimits sqrt;
  MLSingleInputSupportLimits tan;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand dequantizeLinear(MLOperand input,
                             MLOperand scale,
                             MLOperand zeroPoint,
                             optional MLOperatorOptions options = {});
};

dictionary MLQuantizeDequantizeLinearSupportLimits {
  MLTensorLimits input;
  MLTensorLimits scale;
  MLTensorLimits zeroPoint;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLQuantizeDequantizeLinearSupportLimits dequantizeLinear;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand quantizeLinear(MLOperand input,
                           MLOperand scale,
                           MLOperand zeroPoint,
                           optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLQuantizeDequantizeLinearSupportLimits quantizeLinear;
};

dictionary MLEluOptions : MLOperatorOptions {
  double alpha = 1;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand elu(MLOperand input, optional MLEluOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits elu;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand expand(MLOperand input,
                   sequence<[EnforceRange] unsigned long> newShape,
                   optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits expand;
};

dictionary MLGatherOptions : MLOperatorOptions {
  [EnforceRange] unsigned long axis = 0;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand gather(MLOperand input,
                   MLOperand indices,
                   optional MLGatherOptions options = {});
};

dictionary MLGatherSupportLimits {
  MLTensorLimits input;
  MLTensorLimits indices;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLGatherSupportLimits gather;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand gatherElements(MLOperand input,
                           MLOperand indices,
                           optional MLGatherOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLGatherSupportLimits gatherElements;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand gatherND(MLOperand input,
                     MLOperand indices,
                     optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLGatherSupportLimits gatherND;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand gelu(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits gelu;
};

dictionary MLGemmOptions : MLOperatorOptions {
  MLOperand c;
  double alpha = 1.0;
  double beta = 1.0;
  boolean aTranspose = false;
  boolean bTranspose = false;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand gemm(MLOperand a, MLOperand b, optional MLGemmOptions options = {});
};

dictionary MLGemmSupportLimits {
  MLTensorLimits a;
  MLTensorLimits b;
  MLTensorLimits c;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLGemmSupportLimits gemm;
};

enum MLGruWeightLayout {
  "zrn",  // update-reset-new gate ordering
  "rzn"   // reset-update-new gate ordering
};

enum MLRecurrentNetworkActivation {
  "relu",
  "sigmoid",
  "tanh"
};

enum MLRecurrentNetworkDirection {
  "forward",
  "backward",
  "both"
};

dictionary MLGruOptions : MLOperatorOptions {
  MLOperand bias;
  MLOperand recurrentBias;
  MLOperand initialHiddenState;
  boolean resetAfter = true;
  boolean returnSequence = false;
  MLRecurrentNetworkDirection direction = "forward";
  MLGruWeightLayout layout = "zrn";
  sequence<MLRecurrentNetworkActivation> activations;
};

partial interface MLGraphBuilder {
  [Throws]
  sequence<MLOperand> gru(MLOperand input,
                          MLOperand weight,
                          MLOperand recurrentWeight,
                          [EnforceRange] unsigned long steps,
                          [EnforceRange] unsigned long hiddenSize,
                          optional MLGruOptions options = {});
};

dictionary MLGruSupportLimits {
  MLTensorLimits input;
  MLTensorLimits weight;
  MLTensorLimits recurrentWeight;
  MLTensorLimits bias;
  MLTensorLimits recurrentBias;
  MLTensorLimits initialHiddenState;
  MLTensorLimits output0;
  MLTensorLimits output1;
};

partial dictionary MLOpSupportLimits {
  MLGruSupportLimits gru;
};

dictionary MLGruCellOptions : MLOperatorOptions {
  MLOperand bias;
  MLOperand recurrentBias;
  boolean resetAfter = true;
  MLGruWeightLayout layout = "zrn";
  sequence<MLRecurrentNetworkActivation> activations;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand gruCell(MLOperand input,
                    MLOperand weight,
                    MLOperand recurrentWeight,
                    MLOperand hiddenState,
                    [EnforceRange] unsigned long hiddenSize,
                    optional MLGruCellOptions options = {});
};

dictionary MLGruCellSupportLimits {
  MLTensorLimits input;
  MLTensorLimits weight;
  MLTensorLimits recurrentWeight;
  MLTensorLimits hiddenState;
  MLTensorLimits bias;
  MLTensorLimits recurrentBias;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLGruCellSupportLimits gruCell;
};

dictionary MLHardSigmoidOptions : MLOperatorOptions {
  double alpha = 0.2;
  double beta = 0.5;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand hardSigmoid(MLOperand input, optional MLHardSigmoidOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits hardSigmoid;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand hardSwish(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits hardSwish;
};

dictionary MLInstanceNormalizationOptions : MLOperatorOptions {
  MLOperand scale;
  MLOperand bias;
  double epsilon = 1e-5;
  MLInputOperandLayout layout = "nchw";
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand instanceNormalization(
    MLOperand input,
    optional MLInstanceNormalizationOptions options = {});
};

dictionary MLNormalizationSupportLimits {
  MLTensorLimits input;
  MLTensorLimits scale;
  MLTensorLimits bias;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLNormalizationSupportLimits instanceNormalization;
};

dictionary MLLayerNormalizationOptions : MLOperatorOptions {
  MLOperand scale;
  MLOperand bias;
  sequence<[EnforceRange] unsigned long> axes;
  double epsilon = 1e-5;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand layerNormalization(MLOperand input,
                               optional MLLayerNormalizationOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLNormalizationSupportLimits layerNormalization;
};

dictionary MLLeakyReluOptions : MLOperatorOptions {
  double alpha = 0.01;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand leakyRelu(MLOperand input, optional MLLeakyReluOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits leakyRelu;
};

dictionary MLLinearOptions : MLOperatorOptions {
  double alpha = 1;
  double beta = 0;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand linear(MLOperand input, optional MLLinearOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits linear;
};

enum MLLstmWeightLayout {
  "iofg", // input-output-forget-cell gate ordering
  "ifgo"  // input-forget-cell-output gate ordering
};

dictionary MLLstmOptions : MLOperatorOptions {
  MLOperand bias;
  MLOperand recurrentBias;
  MLOperand peepholeWeight;
  MLOperand initialHiddenState;
  MLOperand initialCellState;
  boolean returnSequence = false;
  MLRecurrentNetworkDirection direction = "forward";
  MLLstmWeightLayout layout = "iofg";
  sequence<MLRecurrentNetworkActivation> activations;
};

partial interface MLGraphBuilder {
  [Throws]
  sequence<MLOperand> lstm(MLOperand input,
                           MLOperand weight,
                           MLOperand recurrentWeight,
                           [EnforceRange] unsigned long steps,
                           [EnforceRange] unsigned long hiddenSize,
                           optional MLLstmOptions options = {});
};

dictionary MLLstmSupportLimits {
  MLTensorLimits input;
  MLTensorLimits weight;
  MLTensorLimits recurrentWeight;
  MLTensorLimits bias;
  MLTensorLimits recurrentBias;
  MLTensorLimits peepholeWeight;
  MLTensorLimits initialHiddenState;
  MLTensorLimits initialCellState;
  MLTensorLimits output0;
  MLTensorLimits output1;
  MLTensorLimits output2;
};

partial dictionary MLOpSupportLimits {
  MLLstmSupportLimits lstm;
};

dictionary MLLstmCellOptions : MLOperatorOptions {
  MLOperand bias;
  MLOperand recurrentBias;
  MLOperand peepholeWeight;
  MLLstmWeightLayout layout = "iofg";
  sequence<MLRecurrentNetworkActivation> activations;
};

partial interface MLGraphBuilder {
  [Throws]
  sequence<MLOperand> lstmCell(MLOperand input,
                               MLOperand weight,
                               MLOperand recurrentWeight,
                               MLOperand hiddenState,
                               MLOperand cellState,
                               [EnforceRange] unsigned long hiddenSize,
                               optional MLLstmCellOptions options = {});
};

dictionary MLLstmCellSupportLimits {
  MLTensorLimits input;
  MLTensorLimits weight;
  MLTensorLimits recurrentWeight;
  MLTensorLimits hiddenState;
  MLTensorLimits cellState;
  MLTensorLimits bias;
  MLTensorLimits recurrentBias;
  MLTensorLimits peepholeWeight;
  MLTensorLimits output0;
  MLTensorLimits output1;
};

partial dictionary MLOpSupportLimits {
  MLLstmCellSupportLimits lstmCell;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand matmul(MLOperand a, MLOperand b, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLBinarySupportLimits matmul;
};

enum MLPaddingMode {
  "constant",
  "edge",
  "reflection"
};

dictionary MLPadOptions : MLOperatorOptions {
  MLPaddingMode mode = "constant";
  MLNumber value = 0;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand pad(MLOperand input,
                sequence<[EnforceRange] unsigned long> beginningPadding,
                sequence<[EnforceRange] unsigned long> endingPadding,
                optional MLPadOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits pad;
};

enum MLRoundingType {
  "floor",
  "ceil"
};

dictionary MLPool2dOptions : MLOperatorOptions {
  sequence<[EnforceRange] unsigned long> windowDimensions;
  sequence<[EnforceRange] unsigned long> padding;
  sequence<[EnforceRange] unsigned long> strides;
  sequence<[EnforceRange] unsigned long> dilations;
  MLInputOperandLayout layout = "nchw";
  MLRoundingType outputShapeRounding = "floor";
  sequence<[EnforceRange] unsigned long> outputSizes;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand averagePool2d(MLOperand input, optional MLPool2dOptions options = {});
  [Throws]
  MLOperand l2Pool2d(MLOperand input, optional MLPool2dOptions options = {});
  [Throws]
  MLOperand maxPool2d(MLOperand input, optional MLPool2dOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits averagePool2d;
  MLSingleInputSupportLimits l2Pool2d;
  MLSingleInputSupportLimits maxPool2d;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand prelu(MLOperand input,
                  MLOperand slope,
                  optional MLOperatorOptions options = {});
};

dictionary MLPreluSupportLimits {
  MLTensorLimits input;
  MLTensorLimits slope;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLPreluSupportLimits prelu;
};

dictionary MLReduceOptions : MLOperatorOptions {
  sequence<[EnforceRange] unsigned long> axes;
  boolean keepDimensions = false;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand reduceL1(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceL2(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceLogSum(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceLogSumExp(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceMax(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceMean(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceMin(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceProduct(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceSum(MLOperand input, optional MLReduceOptions options = {});
  [Throws]
  MLOperand reduceSumSquare(MLOperand input, optional MLReduceOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits reduceL1;
  MLSingleInputSupportLimits reduceL2;
  MLSingleInputSupportLimits reduceLogSum;
  MLSingleInputSupportLimits reduceLogSumExp;
  MLSingleInputSupportLimits reduceMax;
  MLSingleInputSupportLimits reduceMean;
  MLSingleInputSupportLimits reduceMin;
  MLSingleInputSupportLimits reduceProduct;
  MLSingleInputSupportLimits reduceSum;
  MLSingleInputSupportLimits reduceSumSquare;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand relu(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits relu;
};

enum MLInterpolationMode {
  "nearest-neighbor",
  "linear"
};

dictionary MLResample2dOptions : MLOperatorOptions {
  MLInterpolationMode mode = "nearest-neighbor";
  sequence<float> scales;
  sequence<[EnforceRange] unsigned long> sizes;
  sequence<[EnforceRange] unsigned long> axes;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand resample2d(MLOperand input, optional MLResample2dOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits resample2d;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand reshape(MLOperand input,
                    sequence<[EnforceRange] unsigned long> newShape,
                    optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits reshape;
};

dictionary MLReverseOptions : MLOperatorOptions {
  sequence<[EnforceRange] unsigned long> axes;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand reverse(MLOperand input, optional MLReverseOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits reverse;
};

dictionary MLScatterOptions : MLOperatorOptions {
  [EnforceRange] unsigned long axis = 0;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand scatterElements(MLOperand input,
                            MLOperand indices,
                            MLOperand updates,
                            optional MLScatterOptions options = {});
};

dictionary MLScatterSupportLimits {
  MLTensorLimits input;
  MLTensorLimits indices;
  MLTensorLimits updates;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLScatterSupportLimits scatterElements;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand scatterND(MLOperand input,
                      MLOperand indices,
                      MLOperand updates,
                      optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLScatterSupportLimits scatterND;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand sigmoid(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits sigmoid;
};

dictionary MLSliceOptions : MLOperatorOptions {
  sequence<[EnforceRange] unsigned long> strides;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand slice(MLOperand input,
                  sequence<[EnforceRange] unsigned long> starts,
                  sequence<[EnforceRange] unsigned long> sizes,
                  optional MLSliceOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits slice;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand softmax(MLOperand input,
                    [EnforceRange] unsigned long axis,
                    optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits softmax;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand softplus(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits softplus;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand softsign(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits softsign;
};

dictionary MLSplitOptions : MLOperatorOptions {
  [EnforceRange] unsigned long axis = 0;
};

partial interface MLGraphBuilder {
  [Throws]
  sequence<MLOperand> split(
      MLOperand input,
      ([EnforceRange] unsigned long or sequence<[EnforceRange] unsigned long>) splits,
      optional MLSplitOptions options = {});
};

dictionary MLSplitSupportLimits {
  MLTensorLimits input;
  MLTensorLimits outputs;
};

partial dictionary MLOpSupportLimits {
  MLSplitSupportLimits split;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand tanh(MLOperand input, optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits tanh;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand tile(MLOperand input,
                 sequence<unsigned long> repetitions,
                 optional MLOperatorOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits tile;
};

dictionary MLTransposeOptions : MLOperatorOptions {
  sequence<[EnforceRange] unsigned long> permutation;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand transpose(MLOperand input, optional MLTransposeOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits transpose;
};

dictionary MLTriangularOptions : MLOperatorOptions {
  boolean upper = true;
  [EnforceRange] long diagonal = 0;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand triangular(MLOperand input, optional MLTriangularOptions options = {});
};

partial dictionary MLOpSupportLimits {
  MLSingleInputSupportLimits triangular;
};

partial interface MLGraphBuilder {
  [Throws]
  MLOperand where(MLOperand condition,
                  MLOperand trueValue,
                  MLOperand falseValue,
                  optional MLOperatorOptions options = {});
};

dictionary MLWhereSupportLimits {
  MLTensorLimits condition;
  MLTensorLimits trueValue;
  MLTensorLimits falseValue;
  MLTensorLimits output;
};

partial dictionary MLOpSupportLimits {
  MLWhereSupportLimits where;
};
