#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage over the generated schema (schema.g.h through schema_ir.h): IR, composition with references,
# registry.json, on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-generated-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all -I"$here/../../../third_party" \
    "$here/registry_generated_test.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" "$here/../registry/report.cpp" \
    -o "$out/registry-generated-test"
"$out/registry-generated-test"
