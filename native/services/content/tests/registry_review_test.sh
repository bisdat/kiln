#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the review findings on the IR reader and composition (registry/ir.cpp, registry/compose.cpp), on
# the host under ASan/UBSan (float-cast-overflow too) and a 1 MiB stack (deep documents must not recurse); then the plan's
# load once more without sanitizers (-O2), for its time.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-review-test (host, sanitizers, 1 MiB stack)"
src=("$here/registry_review_test.cpp" "$here/../registry/ir.cpp" "$here/../registry/compose.cpp")
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
    -I"$here/../../../third_party" "${src[@]}" -o "$out/registry-review-test"
(ulimit -s 1024 && "$out/registry-review-test")
echo "== registry-review-test (host, -O2, no sanitizers: the times)"
g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-function -I"$here/../../../third_party" "${src[@]}" -o "$out/registry-review-test-o2"
"$out/registry-review-test-o2" | grep -E "composed in|x 20 alter|appended and removed|passed"
