#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
source=${KILN_COMPAT_SOURCE:-$here/../registry/entry_compat.cpp}
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$here/../../../third_party" "$here/registry_entry_compat_test.cpp" "$source" "$here/../registry/ir.cpp" -o "$out/registry-entry-compat-test"
"$out/registry-entry-compat-test"
