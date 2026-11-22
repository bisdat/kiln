#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-type-limits -D__fastcall= -ffunction-sections \
    -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/host" -I"$here/.." \
    -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/bootpack_test.cpp" -Wl,--gc-sections \
    -o "$out/bootpack-test"
ASAN_OPTIONS=detect_leaks=0 "$out/bootpack-test"
