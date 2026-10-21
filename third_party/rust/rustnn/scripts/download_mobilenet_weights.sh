#!/bin/bash
# Download all MobileNetV2 pretrained weights from WebNN test-data repository

BASE_URL="https://raw.githubusercontent.com/webmachinelearning/test-data/0495fc5b5e4ccf77f745b747aa43e12a71a30cff/models/mobilenetv2_nchw/weights"
WEIGHTS_DIR="examples/mobilenetv2_weights"

echo "Downloading all MobileNetV2 weights..."
echo "Target directory: $WEIGHTS_DIR"
echo ""

# Create weights directory if it doesn't exist
mkdir -p "$WEIGHTS_DIR"

# Download conv layer weights (even numbers from 0 to 95)
# Based on the WebNN test-data repo structure
CONV_LAYERS=(0 2 4 5 7 9 10 12 14 16 18 20 21 23 25 27 29 31 33 35 37 38 40 42 44 46 48 50 52 54 56 58 60 61 63 65 67 69 71 73 75 77 78 80 82 84 86 88 90 92 94 95)

total=${#CONV_LAYERS[@]}
current=0

for layer in "${CONV_LAYERS[@]}"; do
    current=$((current + 1))
    echo "[$current/$total] Downloading conv_${layer} weights..."

    # Download weight file
    if [ ! -f "$WEIGHTS_DIR/conv_${layer}_weight.npy" ]; then
        curl -s -o "$WEIGHTS_DIR/conv_${layer}_weight.npy" "$BASE_URL/conv_${layer}_weight.npy"
    fi

    # Download bias file
    if [ ! -f "$WEIGHTS_DIR/conv_${layer}_bias.npy" ]; then
        curl -s -o "$WEIGHTS_DIR/conv_${layer}_bias.npy" "$BASE_URL/conv_${layer}_bias.npy"
    fi
done

# Download FC layer (already downloaded, but check)
echo "Checking final classifier weights..."
if [ ! -f "$WEIGHTS_DIR/gemm_104_weight.npy" ]; then
    curl -s -o "$WEIGHTS_DIR/gemm_104_weight.npy" "$BASE_URL/gemm_104_weight.npy"
fi

if [ ! -f "$WEIGHTS_DIR/gemm_104_bias.npy" ]; then
    curl -s -o "$WEIGHTS_DIR/gemm_104_bias.npy" "$BASE_URL/gemm_104_bias.npy"
fi

echo ""
echo "✓ Download complete!"
echo "Total files: $(ls -1 $WEIGHTS_DIR/*.npy | wc -l)"
