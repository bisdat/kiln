#!/usr/bin/env bash
# S8: the transport binding's lifetime (bridge_test) and the REMOTE_VISUAL backend over the real lease model
# (remote_test), under ASan/UBSan and then TSan. Run through the wave's slot runner, never next to another heavy check.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to a dedicated build directory}"
out=$KILN_OUT/bridge-tests
mkdir -p "$out/tmp"
export TMPDIR=$out/tmp
cxx=${CXX:-g++}
common=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I"$here/../../../core/abi")
src=("$here/../bridge.cpp" "$here/../state.cpp")
"$cxx" "${common[@]}" -fsanitize=address,undefined "$here/bridge_test.cpp" "${src[@]}" -o "$out/bridge-asan"
"$out/bridge-asan"
"$cxx" "${common[@]}" -fsanitize=thread "$here/bridge_test.cpp" "${src[@]}" -o "$out/bridge-tsan"
"$out/bridge-tsan"
"$cxx" "${common[@]}" -fsanitize=address,undefined "$here/remote_test.cpp" "${src[@]}" -o "$out/remote-asan"
"$out/remote-asan"
"$cxx" "${common[@]}" -fsanitize=thread "$here/remote_test.cpp" "${src[@]}" -o "$out/remote-tsan"
"$out/remote-tsan"
