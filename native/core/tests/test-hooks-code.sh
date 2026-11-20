#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of the registry behind kiln.hooks' code_state (services/hooks_code.h), with fake file and live readers:
#   native/core/tests/test-hooks-code.sh asan    (ASan/UBSan; default)
#   native/core/tests/test-hooks-code.sh tsan    (ThreadSanitizer)
# Build output: $KILN_OUT/core-tests.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?}/core-tests
mkdir -p "$out"
mode=${1:-asan}
case "$mode" in
  asan) sanitizer=address,undefined ;;
  tsan) sanitizer=thread ;;
  *) printf 'expected asan or tsan\n' >&2; exit 2 ;;
esac
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize="$sanitizer" -fno-sanitize-recover=all \
  -I"$here/../abi" -I"$here/../src" "$here/hooks_code_test.cpp" -o "$out/hooks_code_test-$mode"
# a query that waits forever on the registry's lock hangs the test: the time limit turns that into a failure
timeout 300 "$out/hooks_code_test-$mode"
