#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of the checked patch policy (services/hooks_patch_policy.h): a checked installation is refused before any
# byte is written when a thread could not be checked or stands in the bytes to replace. With ASan/UBSan:
#   native/core/tests/test-hooks-checked-patch.sh
# Build output: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/../abi" -o "$out/hooks_checked_patch_test" "$here/hooks_checked_patch_test.cpp"
"$out/hooks_checked_patch_test"
