#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln.window_clock's state machine (services/window_clock_core.h), with ASan/UBSan:
#   native/core/tests/test-window-clock.sh
# Build output: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/../abi" -o "$out/window_clock_test" "$here/window_clock_test.cpp"
"$out/window_clock_test"
