#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?}/core-tests
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../abi" -I"$here/../src" "$here/hooks_typed_result_test.cpp" -o "$out/hooks_typed_result_test"
"$out/hooks_typed_result_test"
