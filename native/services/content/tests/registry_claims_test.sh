#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the claim index (registry/claims.cpp) answers as today's full scans do, on the host, under a
# 1 MiB stack.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-claims-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all "$here/registry_claims_test.cpp" "$here/../registry/claims.cpp" -o "$out/registry-claims-test"
(ulimit -s 1024 && "$out/registry-claims-test")  # deep paths must not recurse: a 1 MiB stack
