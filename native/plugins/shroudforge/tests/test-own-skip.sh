#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of the ShroudForge mods the profile has off whose folder ShroudForge would still run (src/own_skip.h),
# with ASan/UBSan:
#   native/plugins/shroudforge/tests/test-own-skip.sh
# Build output: $KILN_OUT/sf-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/sf-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/../src" -I"$here/../../../third_party" -o "$out/sf_own_skip_test" "$here/own_skip_test.cpp"
"$out/sf_own_skip_test"
