#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the edit IR and composition (registry/ir.cpp, registry/compose.cpp), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-compose-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/registry_compose_test.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" -o "$out/registry-compose-test"
"$out/registry-compose-test"
