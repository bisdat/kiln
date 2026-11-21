#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of kiln.client's building input (services/client_building_input.cpp):
#   native/core/tests/test-client-building-input.sh asan    (ASan/UBSan; default)
#   native/core/tests/test-client-building-input.sh tsan    (ThreadSanitizer)
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
  -I"$here/../abi" -I"$here/../src" "$here/client_building_input_test.cpp" "$here/../src/services/client_building_input.cpp" \
  -o "$out/client_building_input_test-$mode"
# a producer that waits for the slot deadlocks the test: the time limit turns that into a failure
timeout 300 "$out/client_building_input_test-$mode"
