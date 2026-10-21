# WebNN graph format

The `.webnn` format is a textual representation of the `GraphJson` AST exposed by this crate. It describes
graph structure and constant declarations; external tensor bytes live in sidecars documented in
[External weight format](external-weights.md).

This DSL is a `webnn-graph` interchange format. It is not a file format defined by the W3C WebNN specification.

## Document structure

```webnn
webnn_graph "example" v2 @quantized {
  inputs {
    input: f32[dyn("batch", 8), 4];
  }

  consts {
    bias: f32[] @scalar(0.5);
    bytes: u8[4] @bytes([1, 2, 3, 4]);
    weight: f32[4, 4] @weights("weight");
  }

  nodes {
    sum = add(input, bias, metadata={kind: "example", axes: [1]});
    [left, right] = split(sum, splits=[2, 2], axis=1);
    result = concat(left, right, axis=1);
  }

  outputs { result; }
}
```

A document contains one header followed by any of these blocks:

- `inputs`: named graph inputs and their descriptors.
- `consts`: named constants and their initialization method.
- `nodes`: operation calls and the operands they produce.
- `outputs`: names exported by the graph.

Blocks may be omitted when empty. `#` starts a comment that continues to the end of the line.

## Header and versions

The header is `webnn_graph "name" vN {`. Supported serialized versions are `v1` and `v2`.

- `v1` represents static input dimensions.
- `v2` can preserve bounded dynamic input dimensions as `dyn("name", max_size)`.

The optional `@quantized` annotation records that the graph contains quantized representations. It is metadata
for consumers; it does not alter parsing or tensor bytes.

## Data types and shapes

The text data types map to `GraphJson::DataType` as follows:

| Text | GraphJson |
| --- | --- |
| `f32` | `float32` |
| `f16` | `float16` |
| `i4` | `int4` |
| `u4` | `uint4` |
| `i32` | `int32` |
| `u32` | `uint32` |
| `i64` | `int64` |
| `u64` | `uint64` |
| `i8` | `int8` |
| `u8` | `uint8` |

A shape is a comma-separated list in brackets. `[]` is a known rank-0 scalar, not an unknown shape.

Input dimensions may be static integers or bounded dynamic dimensions. Constants must have fully static shapes.

```webnn
scalar: f32[];
static: f32[1, 128, 768];
dynamic: f32[dyn("batch", 8), dyn("sequence", 4096), 768];
```

Each dynamic dimension has a stable name and a maximum size. Execution consumers decide which concrete sizes
within those bounds they support.

## Inputs and constants

Inputs declare only a name, data type, and shape:

```webnn
inputs {
  input_ids: i64[1, 128];
}
```

Constants use one initialization annotation:

- `@weights("ref")` resolves bytes from an external sidecar. If no annotation is written, the parser uses
  `@weights("constant_name")`.
- `@scalar(value)` stores a JSON number as a scalar initializer.
- `@bytes([0, 1, ...])` stores bytes directly in the graph.

```webnn
consts {
  matrix: f32[4, 4] @weights("encoder.matrix");
  epsilon: f32[] @scalar(0.00001);
  mask: u8[4] @bytes([1, 1, 0, 0]);
}
```

The graph declaration is authoritative for the logical constant name, data type, and shape. See
[External weight format](external-weights.md) for sidecar lookup and storage rules.

## Nodes and values

A single-output operation assigns its result to one identifier:

```webnn
sum = add(lhs, rhs);
```

A multi-output operation declares all output identifiers:

```webnn
[first, second] = split(input, splits=[2, 2], axis=1);
```

Arguments without a name are operand references or literals. Named arguments become entries in the node's
`options` map. Values may be identifiers, strings, numbers, booleans, `null`, arrays, or JSON-like objects.

Identifiers begin with an ASCII letter, `_`, `/`, or `$`. Remaining characters may also include digits and `.`.
Serialized graphs should prefer portable identifier names because downstream consumers may impose stricter rules.

## Outputs

The outputs block lists exported operand names:

```webnn
outputs { logits; hidden_state; }
```

Each name is both the public output binding and the referenced operand name in the text format. `GraphJson`
stores outputs as a map from binding name to operand reference; serializing to `.webnn` emits the map keys.
Consumers that need distinct binding and operand names must normalize them before text serialization.

## GraphJson relationship

The parser maps `.webnn` into these `GraphJson` fields:

- header name, version, and quantized flag;
- ordered maps of inputs and constants;
- an ordered list of nodes;
- an ordered map of output bindings.

`parse_wg_text` parses the DSL. `serialize_graph_to_wg_text` serializes `GraphJson` versions 1 and 2. A graph
that uses constructs representable in the DSL is stable across parse → serialize → parse, modulo formatting and
map ordering.

The maintained [format reference example](../examples/format_reference.webnn) is exercised by an automated
round-trip test.
