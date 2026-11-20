#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?}/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$here/../../../core/abi" "$here/pending_test.cpp" -o "$out/pending-test"
ASAN_OPTIONS=detect_leaks=0 "$out/pending-test"
