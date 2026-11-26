#!/usr/bin/env bash
# kiln.npcspeech's core (what a bubble draw shows, the record's voice-over mute and restore) without a game or Wine.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=/tmp/kiln-npcspeech-tests}"
mkdir -p "$KILN_OUT/npcspeech-tests"
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined \
    -I"$here/.." -I"$here/../../../core/abi" "$here/npcspeech_test.cpp" -o "$KILN_OUT/npcspeech-tests/npcspeech-test"
"$KILN_OUT/npcspeech-tests/npcspeech-test"
