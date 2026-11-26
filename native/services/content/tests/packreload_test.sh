#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content: a mod's data packs follow its load (packreload_test.cpp: content.cpp's Pre, pack_mods and
# pack_mod_reloaded over the real packs.cpp, store.cpp and refl.cpp, a temporary kiln/mods and a made-up item registry),
# on the host. Built twice: as kiln.content alone, and with kiln.gamedata's pack hooks (KILN_CONTENT_GAMEDATA, a model of
# its clones, config adds and other services' sections), so a pack that throws is seen leaving nothing of either behind.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
for variant in content; do
    defs=()
    [ "$variant" = gamedata ] && defs=(-DKILN_CONTENT_GAMEDATA)
    echo "== packreload-test (host, $variant)"
    g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
        -Wno-unused-but-set-variable -Wno-type-limits -Wno-missing-field-initializers -D__fastcall= -ffunction-sections \
        -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined "${defs[@]}" -I"$here/host" -I"$here/.." \
        -I"$here/../../../core/abi" -I"$here/../../../third_party" "$here/packreload_test.cpp" "$here/../packs.cpp" \
        "$here/../store.cpp" "$here/../refl.cpp" "$here/../gear.cpp" -Wl,--gc-sections -pthread -o "$out/packreload-test-$variant"
    ASAN_OPTIONS=detect_leaks=0 "$out/packreload-test-$variant"
done
