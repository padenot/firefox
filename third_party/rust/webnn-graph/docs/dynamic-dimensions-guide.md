# Dynamic dimensions

ONNX inputs may use symbolic or unknown dimensions. `webnn-graph` can either replace them with concrete values
or preserve them as bounded input metadata. Shape-critical operation arguments must still be resolvable during
conversion.

## Static overrides

Provide one or more values directly:

```bash
webnn-graph convert-onnx \
  --input model.onnx \
  --override-dim batch_size=1 \
  --override-dim sequence_length=128
```

Or load an object from a file:

```bash
webnn-graph convert-onnx \
  --input model.onnx \
  --override-dims-file dimensions.json
```

Both accepted JSON shapes are equivalent:

```json
{
  "batch_size": 1,
  "sequence_length": 128
}
```

```json
{
  "freeDimensionOverrides": {
    "batch_size": 1,
    "sequence_length": 128
  }
}
```

Repeated `--override-dim` values are applied after `--override-dims-file` and replace the same key.

## Implicit sources and precedence

The converter resolves dimensions in this order:

1. Values supplied through `--override-dims-file` and `--override-dim`.
2. If no explicit values were supplied, `<model-stem>.dims.json` next to the ONNX file.
3. `freeDimensionOverrides` JSON stored in ONNX model metadata, filling names not already set.
4. Without experimental dynamic inputs, common batch names (`batch_size`, `batch`, `n`, and `b`, matched
   case-insensitively) use an inference value of 1.

Supplying any explicit override prevents automatic loading of the `.dims.json` sidecar. Put the complete desired
set in the explicit source when mixing would otherwise be required.

An unresolved symbolic dimension produces an error naming the input and suggested `--override-dim` flag.
Non-positive and unnamed dimensions use a generated `<input>_dim<index>` hint.

## Bounded dynamic input metadata

Enable experimental preservation with:

```bash
webnn-graph convert-onnx \
  --input model.onnx \
  --experimental-dynamic-inputs
```

Unresolved input dimensions become v2 descriptors such as:

```webnn
inputs {
  input_ids: i64[dyn("batch_size", 8), dyn("sequence_length", 4096)];
}
```

The current default maximum sizes are:

- 4096 for names containing `past`, `seq`, or `length`, and for `s` or `t`;
- 8 for names containing `batch`, and for `b` or `n`;
- 65535 for other symbolic or generated names.

Explicit overrides always produce static dimensions. When experimental preservation is enabled, unresolved batch
names remain bounded dynamic dimensions rather than receiving the static inference default of 1.

If any input contains a dynamic dimension, the converter emits graph version 2. Otherwise it emits version 1.

## Static lowering constraints

Dynamic input metadata does not make every ONNX shape expression dynamic at runtime. Conversion still needs
concrete information for arguments such as reshape targets, slice bounds, axes, permutations, split sizes, and
other operation-specific attributes.

`--optimize` runs the registered constant-folding evaluators before lowering. This can eliminate shape-producing
subgraphs when their inputs are constant:

```bash
webnn-graph convert-onnx \
  --input model.onnx \
  --experimental-dynamic-inputs \
  --optimize
```

If a required value remains unresolved, conversion fails instead of inventing a shape. Add a concrete override
for the relevant symbolic dimension or use a model whose shape-critical expression can be folded.

## Choosing overrides

Choose values from the model's tokenizer, processor, configuration, or deployment contract. Dimension names are
model-defined; inspect the ONNX input descriptors with a tool such as Netron or an ONNX library rather than
assuming a repository-wide list.

Start with the actual production batch and sequence/image/audio sizes. Overrides specialize the converted graph,
so a different deployment shape normally requires another conversion or a supported bounded-dynamic input.

See [ONNX to WebNN lowering](onnx-lowering.md) for the complete conversion flow and
[WebNN graph format](webnn-format.md) for the serialized dynamic-dimension syntax.
