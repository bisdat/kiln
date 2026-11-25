#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln::MidFrames (abi/kiln_mid_frames.hpp: two mid hooks of one call paired by the function's rsp), with
# ASan/UBSan. No Wine and no game:
#   native/core/tests/test-mid-frames.sh
# Build output: $KILN_OUT/mid-frames-test (KILN_OUT default native/build). CXX: the host compiler.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/mid-frames-test
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/../abi" -o "$out/mid_frames_test" "$here/mid_frames_test.cpp"
"$out/mid_frames_test"
