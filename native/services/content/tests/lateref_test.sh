#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: a late kitbash reference is written into an item clone only on the thread of a world that has the clone,
# and a clone the server world has only on the server world's thread (lateref_test.cpp: newcontent.cpp's registration,
# client Pre and server Pre over a fake hosted game), on the host. Then the same file under ThreadSanitizer: a reader
# thread copies the server world's clone while the client world's retry runs.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
       -Wno-unused-but-set-variable -Wno-type-limits -D__fastcall= -ffunction-sections -fdata-sections
       -I"$here/host" -I"$here/.." -I"$here/../../../core/abi" -I"$here/../../../third_party")
echo "== lateref-test (host)"
g++ "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined "$here/lateref_test.cpp" \
    -Wl,--gc-sections -o "$out/lateref-test"
ASAN_OPTIONS=detect_leaks=0 "$out/lateref-test"
echo "== lateref-tsan (host, ThreadSanitizer)"
g++ "${flags[@]}" -DKITBASH_TSAN -fsanitize=thread "$here/lateref_test.cpp" -Wl,--gc-sections -pthread \
    -o "$out/lateref-tsan"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/lateref-tsan"