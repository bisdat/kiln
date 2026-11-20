#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.hooks open_module's PE reading (services/hooks_modules.h) with the host compiler: no Wine, no game.
#   KILN_OUT=<build dir> native/core/tests/test-hooks-modules.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?}/core-tests
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -I"$here/../src" "$here/hooks_modules_test.cpp" -o "$out/hooks_modules_test"
"$out/hooks_modules_test"
