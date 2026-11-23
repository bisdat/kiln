#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
    -I"$here/../../../third_party" "$here/registry_entries_test.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" -o "$out/registry-entries-test"
"$out/registry-entries-test"
