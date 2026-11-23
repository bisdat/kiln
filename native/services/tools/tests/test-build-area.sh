#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through the shared build slot. No game process; output stays outside the source tree.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: test-build-area.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror \
    "$here/build_area_test.cpp" "$here/../hooks/build_area.cpp" -o "$out/build-area-test"
"$out/build-area-test"
