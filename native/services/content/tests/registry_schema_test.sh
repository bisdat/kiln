#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the generated schema (registry/schema.g.h) as the edit IR's descriptor (schema_ir.h), on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-schema-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/registry_schema_test.cpp" "$here/../registry/ir.cpp" -o "$out/registry-schema-test"
"$out/registry-schema-test"
