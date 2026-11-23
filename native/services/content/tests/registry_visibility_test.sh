#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
source=${KILN_VISIBILITY_SOURCE:-$here/../registry/visibility.cpp}
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$here/registry_visibility_test.cpp" "$source" -o "$out/registry-visibility-test"
"$out/registry-visibility-test"
