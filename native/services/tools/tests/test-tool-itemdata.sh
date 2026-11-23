#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through the build limiter. Inputs are a compiler-emitted JSON descriptor, an independently extracted
# TypeDb JSON and the original Small brush ItemInfo resource. Game data stays in assigned scratch, never this tree.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: test-tool-itemdata.sh OUTPUT_DIRECTORY COMPILER_JSON TYPE_DB_JSON BRUSH_BIN}
compiler=${2:?compiler JSON}; types=${3:?TypeDb JSON}; brush=${4:?Small brush resource}
mkdir -p "$out"
export TMPDIR="$out"
python3 -I "$here/make_itemdata_fixture.py" "$types" "$out/brush-types.json"
"${CXX:-c++}" -std=c++20 -O1 -D__fastcall= -ffunction-sections -fdata-sections \
    -I"$here/../../content/tests/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/tool_itemdata_test.cpp" "$here/../../content/refl.cpp" "$here/../../content/gamedata_walk.cpp" \
    -Wl,--gc-sections -o "$out/tool-itemdata-test"
"$out/tool-itemdata-test" "$compiler" "$out/brush-types.json" "$brush"
