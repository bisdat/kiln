#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the regmap (registry/regmap.cpp) on made-up rows and on the repository's regmap/, on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-regmap-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/registry_regmap_test.cpp" "$here/../registry/regmap.cpp" -o "$out/registry-regmap-test"
"$out/registry-regmap-test" "$here/../../../../regmap"
