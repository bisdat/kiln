#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Linux host tests of kiln-dllmod's Windows-free logic (src/package.h). No Wine or game.
#   native/plugins/dllmod/tests/run.sh
# The Windows half (the host that loads and starts test mods) runs under Wine: tests/run_host.sh.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/dllmod-tests
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$here/logic_test.cpp" -o "$out/logic_test"
"$out/logic_test"
