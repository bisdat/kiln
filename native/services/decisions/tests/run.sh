#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of kiln.decisions (wire layout, world gate, broker, metadata cache, death and pickup rules), with ASan/UBSan:
#   KILN_OUT=<build dir> native/services/decisions/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/decisions-host-test
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/../../../core/abi")
g++ "${flags[@]}" "$here/contract_test.cpp" -o "$out/contract_test"
"$out/contract_test"
g++ "${flags[@]}" "$here/world_gate_test.cpp" -o "$out/world_gate_test"
g++ "${flags[@]}" "$here/producer_arm_test.cpp" -o "$out/producer_arm_test"
"$out/producer_arm_test"
"$out/world_gate_test"
g++ "${flags[@]}" "$here/queue_test.cpp" -o "$out/queue_test"
"$out/queue_test"
g++ "${flags[@]}" "$here/death_test.cpp" -o "$out/death_test"
g++ "${flags[@]}" "$here/pickup_test.cpp" -o "$out/pickup_test"
"$out/pickup_test"
"$out/death_test"
g++ "${flags[@]}" "$here/shutdown_test.cpp" -o "$out/shutdown_test"
"$out/shutdown_test"
g++ "${flags[@]}" "$here/reset_test.cpp" -o "$out/reset_test"
"$out/reset_test"
g++ "${flags[@]}" "$here/waiter_test.cpp" -o "$out/waiter_test"
"$out/waiter_test"
g++ "${flags[@]}" "$here/watch_test.cpp" -o "$out/watch_test"
"$out/watch_test"
