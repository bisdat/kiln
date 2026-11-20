#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
vfx=$here/..
: "${KILN_OUT:?set KILN_OUT}"
out=$KILN_OUT/vfx-asset-admission
mkdir -p "$out"
sed -n '/^        kvb::CookOptions opt;$/,/^        opt.game_has_material =/p' \
    "${VFX_BOOT_SOURCE:-$vfx/boot.cpp}" | sed '$d' > "$out/asset_admission.inc"
c++ -std=c++20 -O0 -g0 -Wall -Wextra -I"$vfx" -I"$vfx/../../third_party" -I"$out" \
    "$here/test_asset_admission.cpp" "$vfx/cook/budget.cpp" -o "$out/test_asset_admission"
"$out/test_asset_admission"
