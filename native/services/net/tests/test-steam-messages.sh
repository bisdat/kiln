#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# host tests of kiln.net's Steam peer-to-peer messages (steam_messages.cpp) over a fake steam_api64 table:
#   native/services/net/tests/test-steam-messages.sh asan [race iterations]   (ASan/UBSan; default)
#   native/services/net/tests/test-steam-messages.sh tsan [race iterations]   (ThreadSanitizer)
# Build output: $KILN_OUT/net-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/net-tests
mkdir -p "$out"
mode=${1:-asan}
case "$mode" in
  asan) sanitizer=address,undefined ;;
  tsan) sanitizer=thread ;;
  *) printf 'expected asan or tsan\n' >&2; exit 2 ;;
esac
# -fno-sanitize=function: the fake exports have their own (layout-equal) record types, as a C DLL's would
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize="$sanitizer" -fno-sanitize=function \
  -fno-sanitize-recover=all \
  -I"$here/../../../core/abi" "$here/steam_messages_test.cpp" "$here/../steam_messages.cpp" \
  -o "$out/steam_messages_test-$mode"
# a member that waits for ever (or a deadlocked exit) hangs the test: the time limit turns that into a failure
timeout 600 "$out/steam_messages_test-$mode" "${2:-100000}"
