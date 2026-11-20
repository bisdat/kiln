#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Resource patches checked against a base view of the start's game data (gamedata_bootview.h): the real collector and
# apply with fake boot/assets tables and a fake view, on the host, built with ASan/UBSan and with TSan.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
for san in address,undefined thread; do
    bin=$out/bootview-test-${san%%,*}
    g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -D__fastcall= -pthread \
        -ffunction-sections -fdata-sections -fsanitize=$san -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
        "$here/bootview_test.cpp" "$here/../gear.cpp" -Wl,--gc-sections -o "$bin"
    "$bin"
done
