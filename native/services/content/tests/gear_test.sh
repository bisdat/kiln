#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The data packs' "gear" section and "gearRules" (gear.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/gear_test.cpp" "$here/../gear.cpp" -o "$out/gear-test"
"$out/gear-test"
