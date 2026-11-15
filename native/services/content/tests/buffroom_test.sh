#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content gamedata: the room of Kiln's copy of the buff collection (buffroom_test.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== buffroom-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/host" -I"$here/../../../core/abi" \
    -I"$here/../../../third_party" "$here/buffroom_test.cpp" -Wl,--gc-sections -o "$out/buffroom-test"
"$out/buffroom-test"
