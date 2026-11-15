#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/ui-host-test
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined \
    -I"$here/scroll_host" -I"$here/stubs" -I"$here/../../../core/abi" \
    "$here/menu_scroll_host_test.cpp" -o "$out/menu_scroll_host_test"
"$out/menu_scroll_host_test"
