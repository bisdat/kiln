#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of a profile mod that is off while the engine's own loader runs (../own_off.h and
# ../../greeneye/src/own_off_greeneye.h), with ASan/UBSan. No Wine and no game are needed:
#   native/plugins/compat/tests/test-own-off.sh
# Build output: $KILN_OUT/compat-tests-host (KILN_OUT default native/build). CXX: the host compiler.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/compat-tests-host
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/../../../third_party" -o "$out/own_off_test" "$here/own_off_test.cpp"
"$out/own_off_test"
