#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Linux host tests of Greeneye's actual signature lookup. No Wine or game.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/greeneye-tests
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -I"$here/host" -I"$here/../include" "$here/find_code_test.cpp" -o "$out/find_code_test"
failed=0
for scenario in plain short-hook r11-hook rel32-hook indirect-hook indirect-slot unreadable-slot inside-image-slot inside-pattern mixed-patches duplicate unreadable mismatch truncated fallback-hook changed-file live-patch read-fault allocation-failure zero-tail raw-tail relocations read-plain read-hooked read-later read-first read-dense read-notes kiln-view kiln-view-none; do
    "$out/find_code_test" "$scenario" || failed=1
done
exit "$failed"
