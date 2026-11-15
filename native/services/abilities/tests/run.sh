#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of kiln.abilities' native thunk decisions and the program verifier, with ASan/UBSan:
#   KILN_OUT=<build dir> native/services/abilities/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/abilities-host-test
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -I"$here/.." -I"$here/../../../core/abi")
g++ "${flags[@]}" "$here/natives_test.cpp" -o "$out/natives_test"
"$out/natives_test"
