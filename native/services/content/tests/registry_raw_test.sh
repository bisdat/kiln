#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the edit IR's Advanced tier (raw member paths) and its composition, on the host; reads the
# published IR example (schema/examples/registry-ir.json) with the native reader.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-raw-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/registry_raw_test.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" -o "$out/registry-raw-test"
"$out/registry-raw-test" "$here/../../../../schema/examples/registry-ir.json"
