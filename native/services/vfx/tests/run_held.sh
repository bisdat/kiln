#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT}"
out=$KILN_OUT/vfx-host-test
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/test_held.cpp" -o "$out/test_held"
"$out/test_held"
# Exercise the real portable cooker and the same diagnostic conversion used by boot.cpp.
root=$(cd "$here/../../../.." && pwd)
"$root/tools/vfxcook/native/build.sh" >/dev/null
mapfile -t objects < <(find "$KILN_OUT/vfxcook/CMakeFiles" -name '*.o' ! -name 'main.cpp.o' | sort)
g++ -std=c++23 -O1 -Wall -Wextra -DHELD_COOK_TEST -I"$root/native/core/abi" -I"$root/native/third_party" "$here/test_held.cpp" "${objects[@]}" -o "$out/test_held_cook"
"$out/test_held_cook"
