# Examples

The checked-in examples demonstrate the `.webnn` DSL, raw-weight utilities, JavaScript emitter, and interactive
HTML emitter.

For format details, see:

- [WebNN graph format](../docs/webnn-format.md)
- [External weight format](../docs/external-weights.md)

## Files

- `format_reference.webnn` exercises the maintained v2 grammar and is covered by a round-trip test.
- `resnet_head.webnn` is a small graph whose constants use external references.
- `tensors/` contains tensor metadata. The build script reconstructs untracked raw `.bin` inputs from the
  checked-in reference archive.
- `build_example.sh` creates a manifest, packs a `WGWT` weights file, and emits JavaScript.
- `browser_example.html` is an example host for emitted WebNN builder code.

Some generated artifacts are checked in for inspection. Rerun the script before relying on them after changing
the graph, manifest, tensors, or emitter.

## Parse, serialize, and validate

```bash
cargo run -- parse examples/format_reference.webnn > /tmp/format_reference.json
cargo run -- serialize /tmp/format_reference.json > /tmp/format_reference.webnn
cargo run -- validate /tmp/format_reference.webnn
```

The format-reference graph declares an external tensor for syntax coverage. Parsing and structural validation do
not resolve that sidecar.

## Raw-weight workflow

Run the complete ResNet-head example from the repository root:

```bash
./examples/build_example.sh
```

It performs the equivalent steps:

```bash
cargo run -- create-manifest \
  --input-dir examples/tensors \
  --output examples/resnet_head.manifest.json \
  --endianness little

cargo run -- pack-weights \
  --manifest examples/resnet_head.manifest.json \
  --input-dir examples/tensors \
  --output examples/resnet_head.weights

cargo run -- validate examples/resnet_head.webnn \
  --weights-manifest examples/resnet_head.manifest.json

cargo run -- emit-js examples/resnet_head.webnn > examples/buildGraph.js
```

The packed file begins with the `WGWT` version-1 header. Manifest byte offsets are absolute from the beginning
of that file, including its eight-byte header.

Inspect the archive by unpacking it into a temporary directory:

```bash
cargo run -- unpack-weights \
  --weights examples/resnet_head.weights \
  --manifest examples/resnet_head.manifest.json \
  --output-dir /tmp/resnet_head_tensors
```

## JavaScript and HTML output

Generate WebNN builder JavaScript:

```bash
cargo run -- emit-js examples/resnet_head.webnn > /tmp/build_graph.js
```

Generate a standalone graph visualization:

```bash
cargo run -- emit-html examples/resnet_head.webnn > /tmp/resnet_head.html
```

Executing the generated builder requires a JavaScript environment that implements the WebNN APIs used by the
graph. The emitters do not provide a WebNN runtime.

## ONNX conversion

ONNX conversion is available with the default `onnx` feature:

```bash
cargo run -- convert-onnx \
  --input model.onnx \
  --output model.webnn \
  --weights model.weights \
  --manifest model.manifest.json \
  --override-dim batch_size=1 \
  --optimize
```

See [ONNX to WebNN lowering](../docs/onnx-lowering.md) and
[Dynamic dimensions](../docs/dynamic-dimensions-guide.md) for current conversion behavior.
