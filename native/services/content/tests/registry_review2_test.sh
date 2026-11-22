#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the second review's findings (registry/ir.cpp, compose.cpp, report.cpp) on the host, under
# ASan/UBSan and a 1 MiB stack; then at -O2 without sanitizers, where the plan's budget (under 100 ms) is asserted.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
src=("$here/registry_review2_test.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp" "$here/../registry/report.cpp")
echo "== registry-review2-test (host, sanitizers, 1 MiB stack)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
    -I"$here/../../../third_party" "${src[@]}" -o "$out/registry-review2-test"
(ulimit -s 1024 && "$out/registry-review2-test")
echo "== registry-review2-test (host, -O2, the budget)"
g++ -std=c++20 -O2 -DKILN_BUDGET -Wall -Wextra -Wno-unused-function -I"$here/../../../third_party" "${src[@]}" -o "$out/registry-review2-test-o2"
"$out/registry-review2-test-o2"
