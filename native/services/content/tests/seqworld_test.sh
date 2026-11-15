#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content gamedata: per-world sequence registration, refusal and retry (seqworld_test.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/seqworld_test.cpp" -Wl,--gc-sections -o "$out/seqworld-test"
"$out/seqworld-test"
