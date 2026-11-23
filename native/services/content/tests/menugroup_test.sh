#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: a recipe definition's menuGroup and trackCrafted (menugroup_test.cpp: newcontent.cpp's parser and query writer over the
# fakes of itemfind_test.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== menugroup-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-type-limits -D__fastcall= -ffunction-sections \
    -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/host" -I"$here/.." \
    -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/menugroup_test.cpp" -Wl,--gc-sections \
    -o "$out/menugroup-test"
ASAN_OPTIONS=detect_leaks=0 "$out/menugroup-test"
