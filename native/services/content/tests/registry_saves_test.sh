#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: field edits that change what existing saves hold (registry/saves.cpp), every class of the
# table on the host, with ASan/UBSan.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-saves-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$here/../../../third_party" "$here/registry_saves_test.cpp" "$here/../registry/saves.cpp" "$here/../registry/ir.cpp" \
    "$here/../registry/compose.cpp" -o "$out/registry-saves-test"
"$out/registry-saves-test"
