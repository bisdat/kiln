#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through the build limiter. ToolCompiler's emitted content (ToolCompilerContentTests with KILN_TOOL_CONTENT_FIXTURE)
# through kiln.content's resolvers with the game client's real reflection and data. Game data stays in assigned scratch:
#   python3 -I extract_tool_content_inputs.py <repo>/tools/kfc <game folder> <client exe> <scratch>
#   test-tool-content.sh <out> <compiler.json> <scratch>/types.json <scratch>/brush.bin <scratch>/pool-item.bin <scratch>/registry.bin
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: test-tool-content.sh OUTPUT_DIRECTORY COMPILER_JSON TYPE_DB_JSON BRUSH_BIN POOL_ITEM_BIN REGISTRY_BIN}
compiler=${2:?compiler JSON}; types=${3:?TypeDb JSON}; brush=${4:?Small brush resource}; pool=${5:?pool item resource}; registry=${6:?item registry resource}
mkdir -p "$out"
export TMPDIR="$out"
python3 -I "$here/make_tool_content_fixture.py" "$types" "$out/content-types.json"
"${CXX:-c++}" -std=c++20 -O1 -g -D__fastcall= -ffunction-sections -fdata-sections -fsanitize=address,undefined \
    -I"$here/../../content/tests/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/tool_content_test.cpp" "$here/../../content/refl.cpp" "$here/../../content/gamedata_walk.cpp" \
    -Wl,--gc-sections -o "$out/tool-content-test"
"$out/tool-content-test" "$compiler" "$out/content-types.json" "$brush" "$pool" "$registry"
