#!/usr/bin/env bash
# Builds and runs kiln.vfx's host test: the queue and the guard's logic against the simulated game. No game, no Wine.
#   native/services/vfx/tests/run.sh [real-guids.txt]     (a file of real VFX GUIDs is local data, never in the repo)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/vfx-host-test
mkdir -p "$out"
g++ -std=c++20 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined -pthread -I"$here/../../../core/abi" \
    "$here/test_core.cpp" "$here/../core.cpp" "$here/../sim.cpp" -o "$out/test_core"
"$out/test_core" "$@"
# the resource budget and the result-bearing definitions (ledger.cpp; package 17a S2): no game, files in a temporary folder
g++ -std=c++20 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined -pthread -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/test_ledger.cpp" "$here/../ledger.cpp" -o "$out/test_ledger"
"$out/test_ledger" "$out/ledger-files"
# the limits of a mod's blocks/generated/*.json (generated.h)
g++ -std=c++20 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined "$here/test_generated.cpp" -o "$out/test_generated"
"$out/test_generated"

bash "$here/run_held.sh"

bash "$here/run_boot_build.sh"

bash "$here/run_observer.sh"
