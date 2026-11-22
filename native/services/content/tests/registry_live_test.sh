#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: the registry's live back-end (registry_live_test.cpp) over the real store, IR, composition, formulas and the
# committed regmap, on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-live-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    -DKILN_REGMAP_DIR="\"$here/../../../../regmap\"" \
    "$here/registry_live_test.cpp" "$here/../refl.cpp" "$here/../gear.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" \
    "$here/../registry/formulas.cpp" "$here/../registry/regmap.cpp" "$here/../registry/report.cpp" -Wl,--gc-sections -o "$out/registry-live-test"
ASAN_OPTIONS=detect_leaks=0 "$out/registry-live-test"
