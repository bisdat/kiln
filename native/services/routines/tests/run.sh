#!/usr/bin/env bash
# kiln.routines on the host (no game, no Wine): the plan reader, the hooks against a stubbed villager worker, and the
# change queue, with address and undefined-behaviour sanitizers; then the hooks again under the thread sanitizer.
#   [KILN_OUT=<build dir>] native/services/routines/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=/tmp/kiln-routines-tests}"
out=$KILN_OUT/routines-tests
mkdir -p "$out"
src=("$here/routines_test.cpp" "$here/../plan.cpp" "$here/../hooks.cpp" "$here/../state.cpp")
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/.." -I"$here/../../../core/abi" "${src[@]}" -o "$out/routines-test"
"$out/routines-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=thread \
    -I"$here/.." -I"$here/../../../core/abi" "${src[@]}" -o "$out/routines-test-tsan"
"$out/routines-test-tsan" threads
