#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of a native mod the profile has off whose DLL also loads as a chain proxy (src/chain_copy.h), with
# ASan/UBSan. No Wine and no game are needed:
#   native/core/tests/test-chain-copy.sh
# Build output: $KILN_OUT/chain-copy-test (KILN_OUT default native/build). CXX: the host compiler.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/chain-copy-test
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/../src" -o "$out/chain_copy_test" "$here/chain_copy_test.cpp"
"$out/chain_copy_test"
