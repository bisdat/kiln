#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: the drop-table hook patches a newly loaded table before any caller walks it, whichever thread acquires
# it first (droptable_test.cpp: content.cpp's note_table over a fake game), on the host, then under ThreadSanitizer.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== droptable-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-type-limits -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/host" -I"$here/.." \
    -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/droptable_test.cpp" -Wl,--gc-sections -pthread \
    -o "$out/droptable-test"
ASAN_OPTIONS=detect_leaks=0 "$out/droptable-test"
echo "== droptable-tsan (host, ThreadSanitizer)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-type-limits -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=thread -I"$here/host" -I"$here/.." -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/droptable_test.cpp" -Wl,--gc-sections -pthread -o "$out/droptable-tsan"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/droptable-tsan"