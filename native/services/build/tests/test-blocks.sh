#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host policy tests only. Windows engine-boundary tests: CMake targets build-block-ops/build-block-engine.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT to the assigned output directory}/build-blocks
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/../../../core/abi" "$here/block_ops_test.cpp" "$here/../block_ops.cpp" -o "$out/block-ops"
"$out/block-ops"
