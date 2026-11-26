#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: the registry line review's findings on the live back-end and the store (registry_live_review_test.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
. "$here/regmap_fixture.sh"
regmap=$(regmap_fixture "$out")  # every row of the build records ok: the back-end, not the proof
echo "== registry-live-review-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    -DKILN_REGMAP_DIR="\"$regmap\"" \
    "$here/registry_live_review_test.cpp" "$here/../packs.cpp" "$here/../refl.cpp" "$here/../gear.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" "$here/../registry/saves.cpp" \
    "$here/../registry/formulas.cpp" "$here/../registry/regmap.cpp" -Wl,--gc-sections -o "$out/registry-live-review-test"
ASAN_OPTIONS=detect_leaks=0 timeout 30s "$out/registry-live-review-test"
