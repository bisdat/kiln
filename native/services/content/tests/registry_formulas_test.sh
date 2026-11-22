#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the Kiln formulas (registry/formulas.cpp over gear.cpp), on the host; the damage rows
# against the committed server type dump.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
repo=$here/../../../..
echo "== registry-formulas-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/registry_formulas_test.cpp" "$here/../registry/formulas.cpp" "$here/../gear.cpp" -o "$out/registry-formulas-test"
"$out/registry-formulas-test" "$repo/sigdb/types/server/f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2.json" "$repo/regmap/common/items.json"
