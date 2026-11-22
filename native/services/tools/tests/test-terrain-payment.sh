#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Offline detour tests. Run through the shared build limiter; never runs a game.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: test-terrain-payment.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I"$here/../../../core/abi" -I"$here/../include" \
    "$here/terrain_payment_test.cpp" "$here/../hooks/terrain_payment.cpp" -o "$out/terrain-payment-test"
"$out/terrain-payment-test"
