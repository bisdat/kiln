#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/gameplay-host-test
mkdir -p "$out"
g++ -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
    -I"$here/../../../core/abi" "$here/component_change_test.cpp" -o "$out/component_change_test"
"$out/component_change_test"
g++ -std=c++20 -D__fastcall= -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
    -I"$here/../../../core/abi" "$here/build_hooks_test.cpp" -o "$out/build_hooks_test"
"$out/build_hooks_test"
"$out/build_hooks_test" failure
"$out/build_hooks_test" immediate
"$out/build_hooks_test" hosted
"$out/build_hooks_test" precheck
"$out/build_hooks_test" unobserved
"$out/build_hooks_test" zones
