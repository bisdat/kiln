#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the claim index (registry/claims.cpp) answers as today's full scans do, on the host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-claims-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/registry_claims_test.cpp" "$here/../registry/claims.cpp" -o "$out/registry-claims-test"
"$out/registry-claims-test"
