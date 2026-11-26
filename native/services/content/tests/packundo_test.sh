#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: a pack that throws partway leaves nothing of its "add" lists behind (packundo_test.cpp: newcontent.cpp's pack
# journal, nc_pack_mark / nc_pack_rollback / nc_pack_unmark, over its real definitions and index, no world), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== packundo-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-type-limits -D__fastcall= -ffunction-sections \
    -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/host" -I"$here/.." \
    -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/packundo_test.cpp" -Wl,--gc-sections \
    -o "$out/packundo-test"
ASAN_OPTIONS=detect_leaks=0 "$out/packundo-test"
