#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/motionpack-host
mkdir -p "$out"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$here/.." "$here/motionpack_test.cpp" "$here/../motionpack.cpp" -o "$out/motionpack_test"
ASAN_OPTIONS=detect_leaks=0 "$out/motionpack_test"
