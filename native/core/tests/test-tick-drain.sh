#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of the core's world-stop drain per world kind (src/tick_drain.h) and of hk_world_destroy's use of it, with
# ASan/UBSan:
#   native/core/tests/test-tick-drain.sh
# Build output: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/../abi" -o "$out/tick_drain_test" "$here/tick_drain_test.cpp"
"$out/tick_drain_test" "$here/../src/tick.cpp"
