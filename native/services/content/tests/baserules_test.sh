#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Kiln's base rules (baserules.hpp) in the real resource-patch collector and apply, with fake boot/assets tables and a
# fake balancing table, on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/baserules_test.cpp" "$here/../gear.cpp" -Wl,--gc-sections -o "$out/baserules-test"
"$out/baserules-test"
