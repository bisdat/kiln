#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.stationmenus (menulayout.cpp): the store of station menu layouts and the crafting-menu trees built from them
# (menulayout_test.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== menulayout-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/host" -I"$here/.." -I"$here/../../../core/abi" \
    -I"$here/../../../third_party" "$here/menulayout_test.cpp" -Wl,--gc-sections -o "$out/menulayout-test"
"$out/menulayout-test"
