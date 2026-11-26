#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content gamedata: a pack that throws partway leaves nothing of its gamedata behind (gdundo_test.cpp: the pack journal
# of gamedata.h over the real gamedata_clones.cpp and gamedata_packs.cpp, no world, a fake service section), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== gdundo-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers -D__fastcall= \
    -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/host" -I"$here/.." \
    -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/gdundo_test.cpp" -Wl,--gc-sections -o "$out/gdundo-test"
ASAN_OPTIONS=detect_leaks=0 "$out/gdundo-test"
