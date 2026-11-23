#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of the check before ShroudForge's own windows start (src/helper_version.h), with ASan/UBSan:
#   native/plugins/shroudforge/tests/test-helper-version.sh
# Build output: $KILN_OUT/sf-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/sf-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/../src" -o "$out/sf_helper_version_test" "$here/helper_version_test.cpp"
"$out/sf_helper_version_test"
