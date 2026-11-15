#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$KILN_OUT/vfx-pending
mkdir -p "$out"
g++ -std=c++20 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined "$here/test_pending.cpp" "$here/../pending_cleanup.cpp" -o "$out/test_pending"
"$out/test_pending" "$out/files"
