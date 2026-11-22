#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: registry.json (registry/report.cpp) over compose()'s result, on the host. The document it builds
# is left in $KILN_OUT/content-tests/registry-report.json for a schema check (schema/state-registry.schema.json).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-report-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I"$here/../../../third_party" \
    "$here/registry_report_test.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" "$here/../registry/report.cpp" \
    -o "$out/registry-report-test"
"$out/registry-report-test" "$out/registry-report.json"
