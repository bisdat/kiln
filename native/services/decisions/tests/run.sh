#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of kiln.decisions (wire layout, broker, metadata cache), with ASan/UBSan:
#   KILN_OUT=<build dir> native/services/decisions/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/decisions-host-test
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/../../../core/abi")
g++ "${flags[@]}" "$here/contract_test.cpp" -o "$out/contract_test"
"$out/contract_test"
g++ "${flags[@]}" "$here/queue_test.cpp" -o "$out/queue_test"
"$out/queue_test"
