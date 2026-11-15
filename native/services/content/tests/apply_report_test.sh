#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?}/content-tests
mkdir -p "$out"
g++ -std=c++20 -O1 -g -D__fastcall= -ffunction-sections -fdata-sections -fsanitize=address,undefined -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/apply_report_test.cpp" -Wl,--gc-sections -o "$out/apply-report-test"
ASAN_OPTIONS=detect_leaks=0 "$out/apply-report-test"
