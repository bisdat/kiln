#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of kiln.playerstate's pure parts (the client action leases), with ASan/UBSan:
#   KILN_OUT=<build dir> native/services/players/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/players-host-test
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined)
g++ "${flags[@]}" "$here/client_leases_test.cpp" -o "$out/client_leases_test"
"$out/client_leases_test"
