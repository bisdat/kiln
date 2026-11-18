#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of the level kiln-shroudforge gives ShroudForge's log lines in kiln.log (src/log_level.h), with ASan/UBSan:
#   native/plugins/shroudforge/tests/test-log-level.sh
# Build output: $KILN_OUT/sf-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/sf-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/../src" -I"$here/../../../core/abi" -o "$out/sf_log_level_test" "$here/log_level_test.cpp"
"$out/sf_log_level_test"
