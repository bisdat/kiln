#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# A new recipe's "water" (recipe_water.h, used by newcontent.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/recipe_water_test.cpp" -o "$out/recipe-water-test"
"$out/recipe-water-test"
