#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?}/original-code
mkdir -p "$out"
for test in original_code iat; do
    "${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/${test}_test.cpp" -o "$out/$test"
done
failed=0
for test in original_code iat; do
    "$out/$test" || failed=1
done
exit "$failed"
