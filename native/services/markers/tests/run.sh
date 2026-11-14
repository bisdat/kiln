#!/usr/bin/env bash
# kiln.markers' host tests: no game, no Wine. The v1 ABI (sizes and offsets; the header compiles as C11 and C++).
#   KILN_OUT=<build dir> native/services/markers/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
abi=$here/../../../core/abi
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/markers-host-test
mkdir -p "$out"

# The header is C: it compiles as C11 (its own size asserts included).
printf '#include "services/kiln_markers.h"\n_Static_assert(KILN_MARKER_WORLD == 4, "surfaces");\n' |
  gcc -std=c11 -Wall -Wextra -pedantic -fsyntax-only -I"$abi" -x c -
echo "PASS kiln_markers.h compiles as C11"

g++ -std=c++20 -O1 -g -Wall -Wextra -I"$abi" "$here/abi_test.cpp" -o "$out/abi_test"
"$out/abi_test"
