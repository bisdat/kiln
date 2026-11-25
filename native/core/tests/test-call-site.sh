#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln::game_call_through_foreign (abi/kiln_call_site.hpp: the game's call behind another module's code), with
# ASan/UBSan. No Wine and no game:
#   native/core/tests/test-call-site.sh
# Build output: $KILN_OUT/call-site-test (KILN_OUT default native/build). CXX: the host compiler.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/call-site-test
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/../abi" -o "$out/call_site_test" "$here/call_site_test.cpp"
"$out/call_site_test"
