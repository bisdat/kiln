#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# host tests of kiln.net's portable parts (no game, no Wine): the pairing of the game's session message calls
# (session_io.cpp: every read freed, every allocation committed or aborted) and the outbound gate and retry queue
# (outbound.cpp: nothing sent once the game's shutdown begins; bounded retries), under ASan/UBSan and then TSan.
#   native/services/net/tests/test-net-host.sh
# Build output: $KILN_OUT/net-host-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/net-host-tests
mkdir -p "$out"
cxx=${CXX:-clang++}
flags=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fno-sanitize-recover=all)
"$cxx" "${flags[@]}" -fsanitize=address,undefined "$here/session_io_test.cpp" "$here/../session_io.cpp" -o "$out/session_io_test"
"$out/session_io_test"
"$cxx" "${flags[@]}" -fsanitize=address,undefined "$here/outbound_test.cpp" "$here/../outbound.cpp" -o "$out/outbound_test-asan"
timeout 600 "$out/outbound_test-asan"
"$cxx" "${flags[@]}" -fsanitize=thread "$here/outbound_test.cpp" "$here/../outbound.cpp" -o "$out/outbound_test-tsan"
timeout 600 "$out/outbound_test-tsan"
