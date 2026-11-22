#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: registry layers in the store (registry_store_test.cpp: the seq band, evaluated layers, compare-before-write), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-store-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/registry_store_test.cpp" -Wl,--gc-sections -o "$out/registry-store-test"
"$out/registry-store-test"
