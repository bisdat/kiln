#!/usr/bin/env bash
# kiln.camera without a game or Wine: the header as C (its layout and numbers), and the lease model on a fake backend
# (who holds an endpoint, the order of answers and events, owners and worlds that go).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
abi=$here/../../../core/abi
: "${KILN_OUT:=/tmp/kiln-camera-tests}"
out=$KILN_OUT/camera-tests
mkdir -p "$out"
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I"$abi" "$here/abi_test.c" -o "$out/abi-test"
"$out/abi-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/state_test.cpp" "$here/../state.cpp" -o "$out/state-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/mode_test.cpp" "$here/../mode.cpp" "$here/../state.cpp" -o "$out/mode-test"
"$out/mode-test"
"$out/state-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/mode_test.cpp" "$here/../mode.cpp" "$here/../state.cpp" -o "$out/mode-test"
"$out/mode-test"
