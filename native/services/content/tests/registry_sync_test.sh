#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Registry live edits through ContentSync's real native export (synthetic game memory).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
. "$here/regmap_fixture.sh"
regmap=$(regmap_fixture "$out")
echo "== registry-sync-test (host)"
flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -D__fastcall= -ffunction-sections -fdata-sections
       -fsanitize=address,undefined -fno-sanitize-recover=all -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party")
# registry_live_test supplies an in-memory section reader; this copy of packs.cpp contributes only export_changes_json.
g++ "${flags[@]}" -Dregistry_sections=sync_pack_registry_sections -Dregistry_schema=sync_pack_registry_schema \
    -c "$here/../packs.cpp" -o "$out/registry-sync-packs.o"
g++ "${flags[@]}" -DKILN_REGMAP_DIR="\"$regmap\"" \
    "$here/registry_sync_test.cpp" "$here/../refl.cpp" "$here/../gear.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" \
    "$here/../registry/formulas.cpp" "$here/../registry/regmap.cpp" "$here/../registry/report.cpp" "$out/registry-sync-packs.o" \
    -Wl,--gc-sections -o "$out/registry-sync-test"
ASAN_OPTIONS=detect_leaks=0 "$out/registry-sync-test"
