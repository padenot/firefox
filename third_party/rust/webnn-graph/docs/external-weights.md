# External weight format

A `GraphJson` constant with `ConstInit::Weights { ref }`, rendered in `.webnn` as `@weights("ref")`, obtains
its bytes from a sidecar. The graph declaration remains authoritative for the logical data type and shape.

Two sidecar families are supported:

- a self-describing SafeTensors archive;
- an opaque `.weights` byte blob addressed by a JSON manifest.

## Discovery

An explicit weights path is resolved relative to the graph file. Without one, discovery checks in order:

1. `<graph-stem>.safetensors`
2. `<graph-stem>.weights`
3. `model.safetensors`
4. `model.weights`

The first existing path wins. A SafeTensors path ignores the manifest argument. A `.weights` path requires an
explicit manifest or a discovered `<graph-stem>.manifest.json` or `manifest.json`.

Exact tensor names take precedence. As a compatibility fallback, archive and manifest names are normalized by
replacing `::` with `__` and `.` with `_`. Resolution fails when more than one stored name normalizes to the
same requested reference.

## Loading and ownership

SafeTensors and raw `.weights` files are read into memory while references are resolved. Every selected tensor
range is then copied into the owned `InlineBytes` representation used by `GraphJson` consumers. The format does
not provide decode-time weight streaming.

Callers must keep a sidecar immutable while it is being resolved. Producers should complete a temporary file
and rename it into place rather than modifying an installed archive.

## SafeTensors

Ordinary logical types use the corresponding SafeTensors storage type:

| Graph declaration | SafeTensors storage |
| --- | --- |
| `float32` | F32 |
| `float16` | F16 |
| `int32` | I32 |
| `uint32` | U32 |
| `int64` | I64 |
| `uint64` | U64 |
| `int8` | I8 |
| `uint8` | U8 |

A BF16 tensor may satisfy a `float32` declaration. The loader converts its values to F32. Other dtype
mismatches are rejected, and ordinary tensor shapes must match the graph declaration exactly.

### Packed Int4 and Uint4

SafeTensors has no native 4-bit storage type. Logical `int4` and `uint4` declarations use this versioned
extension:

- archive metadata key: `rustnn.webnn.packed4`
- supported metadata value: `1`
- physical SafeTensors type: U8
- physical shape: `[ceil(logical_element_count / 2)]`
- byte layout: the first logical element occupies the low nibble and the second occupies the high nibble

The `.webnn` or `GraphJson` declaration retains the logical 4-bit dtype and original shape. The loader requires
the marker whenever an archive is used for an external 4-bit declaration. It rejects a missing or unknown
marker, non-U8 storage, an incorrect physical shape or byte count, overflow, and missing references.

Archives containing only ordinary tensors need no packed-4-bit marker and remain compatible with earlier files.

## Manifest-backed `.weights`

For raw weights, the manifest contains a `tensors` object keyed by each `@weights` reference. Resolution uses
`byteOffset` and `byteLength` as an absolute half-open byte range from the beginning of the `.weights` file.

The weight utilities use this version-1 manifest shape:

```json
{
  "format": "wg-weights-manifest",
  "version": 1,
  "endianness": "little",
  "tensors": {
    "weight": {
      "dataType": "float32",
      "shape": [2, 2],
      "byteOffset": 8,
      "byteLength": 16,
      "layout": "row-major"
    }
  }
}
```

The external resolver accepts related manifest layouts as long as `tensors.<name>.byteOffset` and
`byteLength` are present. It validates integer conversion, range addition, and file bounds. Graph dtype and
shape remain authoritative during resolution; the richer manifest fields are consumed by the pack/unpack tools.

Two current producers use absolute offsets differently:

- `pack-weights` and `extract-weights` write `WGWT`, followed by a little-endian U32 version (`1`), then tensor
  bytes. Their first tensor offset is 8. `unpack-weights` and `inline-weights` validate this header.
- `convert-onnx` writes a headerless concatenation whose first tensor offset is 0.

Because offsets are absolute, the shared external resolver can load both layouts without interpreting a header.

## SafeTensors writer

`write_external_weights_safetensors` accepts a `GraphJson`, bytes keyed by weight reference, and a destination
path. It rejects missing bytes, conflicting declarations, unsupported mappings, shape/length mismatches, and
packed element-count overflow. It adds the packed-4-bit marker only when the graph contains external Int4 or
Uint4 tensors.

The writer calls `safetensors::serialize_to_file` for a uniquely named temporary file in the destination
directory and renames it only after successful serialization. Failed serialization or installation removes the
temporary file and does not install a partial final archive.
