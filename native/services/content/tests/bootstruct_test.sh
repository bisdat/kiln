#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The structural edits of a boot resource patch on hand-made bytes and types, on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/bootstruct_test.cpp" -Wl,--gc-sections -o "$out/bootstruct-test"
"$out/bootstruct-test"
