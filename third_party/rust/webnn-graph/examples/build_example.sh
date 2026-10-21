#!/bin/bash
# Complete workflow example for WebNN graph with weights

set -e

# Change to repository root
cd "$(dirname "$0")/.."

TENSOR_DIR="examples/tensors"
TEMP_DIR=""
if [ ! -f "$TENSOR_DIR/W.bin" ] || [ ! -f "$TENSOR_DIR/b.bin" ]; then
  TEMP_DIR=$(mktemp -d)
  trap 'rm -rf "$TEMP_DIR"' EXIT
  TENSOR_DIR="$TEMP_DIR/tensors"
  mkdir -p "$TENSOR_DIR"

  echo "Raw tensor inputs are not checked in; unpacking the reference archive..."
  cargo run --quiet -- unpack-weights \
    --weights examples/resnet_head.weights \
    --manifest examples/resnet_head.manifest.json \
    --output-dir "$TENSOR_DIR"
  cp examples/tensors/*.meta.json "$TENSOR_DIR/"
fi

echo "=== WebNN Graph Complete Workflow Example ==="
echo

# Step 1: Create manifest from tensor directory
echo "Step 1: Creating weights manifest from tensors..."
cargo run --quiet -- create-manifest \
  --input-dir "$TENSOR_DIR" \
  --output examples/resnet_head.manifest.json \
  --endianness little
echo

# Step 2: Pack weights into binary file
echo "Step 2: Packing weights into binary format..."
cargo run --quiet -- pack-weights \
  --manifest examples/resnet_head.manifest.json \
  --input-dir "$TENSOR_DIR" \
  --output examples/resnet_head.weights
echo

# Step 3: Parse graph and emit JavaScript
echo "Step 3: Generating JavaScript code..."
cargo run --quiet -- emit-js examples/resnet_head.webnn > examples/buildGraph.js
echo "Generated examples/buildGraph.js"
echo

# Step 4: Show file sizes
echo "=== Generated Files ==="
ls -lh examples/resnet_head.manifest.json examples/resnet_head.weights examples/buildGraph.js | awk '{print $9, "-", $5}'
echo

echo "See examples/README.md for parse, serialize, validation, emitter, and unpack commands."
