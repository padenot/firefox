#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""
Generate an assembly file that embeds Metal shader source for runtime compilation.
This allows cross-compilation from Linux without requiring the Metal compiler (xcrun).
"""

import os
import sys

def main(output, *args):
    """
    Generate assembly file with embedded Metal shader source.

    Args:
        output: Output .s file (FileAvoidWrite object from Mozilla build system)
        *args: Additional arguments (unused)
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Input file paths
    common_h_path = os.path.join(script_dir, 'ggml', 'src', 'ggml-common.h')
    impl_h_path = os.path.join(script_dir, 'ggml', 'src', 'ggml-metal', 'ggml-metal-impl.h')
    metal_src_path = os.path.join(script_dir, 'ggml', 'src', 'ggml-metal', 'ggml-metal.metal')

    # Read source files
    with open(common_h_path, 'r', encoding='utf-8') as f:
        common_h = f.read()

    with open(impl_h_path, 'r', encoding='utf-8') as f:
        impl_h = f.read()

    with open(metal_src_path, 'r', encoding='utf-8') as f:
        metal_src = f.read()

    # Replace placeholders to merge headers into Metal source
    # This mimics what the CMake build does with sed commands
    metal_src = metal_src.replace('__embed_ggml-common.h__', common_h)
    metal_src = metal_src.replace('#include "ggml-metal-impl.h"', impl_h)

    # Get the actual output path from the FileAvoidWrite object
    # In Mozilla's build system, output is a file-like object with a .name attribute
    output_path = output.name if hasattr(output, 'name') else str(output)
    output_dir = os.path.dirname(os.path.abspath(output_path))
    merged_metal_path = os.path.join(output_dir, 'ggml-metal-embed.metal')

    # Write merged Metal source to a temporary file
    # We need an actual file for .incbin to reference
    with open(merged_metal_path, 'w', encoding='utf-8') as f:
        f.write(metal_src)

    # Generate assembly file that embeds the Metal source
    # The symbols ggml_metallib_start and ggml_metallib_end will be
    # referenced from ggml-metal.m to access the embedded shader source
    asm_content = f'''.section __DATA,__ggml_metallib
.globl _ggml_metallib_start
_ggml_metallib_start:
.incbin "{merged_metal_path}"
.globl _ggml_metallib_end
_ggml_metallib_end:
'''

    # Write assembly file using the file-like object
    output.write(asm_content)

    print(f"Generated {output_path} with embedded Metal shader source ({len(metal_src)} bytes)")
    return 0

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: generate_metal_embed.py <output.s>", file=sys.stderr)
        sys.exit(1)

    sys.exit(main(sys.argv[1], *sys.argv[2:]))