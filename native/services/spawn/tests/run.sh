#!/usr/bin/env bash
# kiln.spawn's host checks: the mid-step gate for create and destroy (midstep.h) and that spawn.cpp's entry points use
# it. No game or Wine.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=/tmp/kiln-spawn-tests}"
mkdir -p "$KILN_OUT/spawn-tests"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -I"$here/../../../core/abi" -I"$here/.." "$here/midstep_test.cpp" -o "$KILN_OUT/spawn-tests/midstep-test"
"$KILN_OUT/spawn-tests/midstep-test" "$here/../spawn.cpp"
# The tracker sidecar on a slow disk (0.2.0-dev.29.17): the tick's calls never wait for it (tests/host/windows.h).
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" -I"$here/.." \
    "$here/tracker_test.cpp" "$here/../tracker.cpp" -o "$KILN_OUT/spawn-tests/tracker-test"
"$KILN_OUT/spawn-tests/tracker-test" "$KILN_OUT/spawn-tests/tracker-files"
