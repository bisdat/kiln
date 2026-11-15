#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/flame-host
mkdir -p "$out"
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
  -I"$here/host" -I"$here/../../../core/abi" "$here/flame_test.cpp" -o "$out/flame_test"
ASAN_OPTIONS=detect_leaks=0 "$out/flame_test"
# the roster (package 17c): roster_core.h and the builder's admission, with the sanitizers and with threads
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
  -I"$here/.." -I"$here/../../../core/abi" "$here/roster_test.cpp" "$here/../questbuild.cpp" -o "$out/roster_test"
ASAN_OPTIONS=detect_leaks=0 "$out/roster_test"
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=thread \
  -I"$here/.." -I"$here/../../../core/abi" "$here/roster_test.cpp" "$here/../questbuild.cpp" -o "$out/roster_test_tsan"
"$out/roster_test_tsan"
