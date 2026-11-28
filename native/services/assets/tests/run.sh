#!/usr/bin/env bash
# Builds and runs kiln.assets' host test: the real store (store.cpp, catalog.cpp, util.cpp) with the game faked at the calls hooks.cpp
# makes for it, on the pin (KILN_ASSET_PUT_PIN), the player graph's reservation, blob provenance and the content blob catalog
# (PASS assets.catalog-snapshot), and a native owner the core revokes without revert_owner (PASS assets.revoked-owner-cleanup).
# No game, no Wine.
#   native/services/assets/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/assets-host-test
mkdir -p "$out"

# The service's header is C: it compiles as C11.
printf '#include <stddef.h>\n#include "services/kiln_assets.h"\n#include "services/kiln_assets_vfx.h"\n_Static_assert(KILN_ASSET_PUT_PIN == 4, "KILN_ASSET_PUT_PIN");\n_Static_assert(sizeof(KilnBlobCatalogInfo) == 32, "info");\n_Static_assert(sizeof(KilnBlobCatalogEntry) == 48, "entry");\n_Static_assert(sizeof(KilnAssetsVfxBudget) == 16, "vfx budget");\n_Static_assert(offsetof(KilnAssetsApi, texture_rgba) == offsetof(KilnAssetsApi, blob_catalog_close) + sizeof(void*), "texture_rgba appended");\n_Static_assert(offsetof(KilnAssetsApi, texture_rgba) == 216 && offsetof(KilnAssetsApi, blob_read_resident) == 224, "x64 layout");\n_Static_assert(sizeof(KilnLoadedInfo) == 24 && offsetof(KilnLoadedInfo, bytes) == 16, "loaded info");\n_Static_assert(offsetof(KilnAssetsApi, loaded_snapshot) == 232 && offsetof(KilnAssetsApi, loaded_list) == 240 && offsetof(KilnAssetsApi, loaded_scalar_compare_exchange) == 248 && sizeof(KilnAssetsApi) == 256, "x64 layout, loaded resources appended");\n' |
  gcc -std=c11 -Wall -Wextra -pedantic -fsyntax-only -I"$here/../../../core/abi" -x c -
echo "PASS kiln_assets.h compiles as C11"

# Compile the production index-copy function with only its game-memory seams replaced.
sed -n '/^GameIndex game_index_keys(/,/^std::string game_container_path/p' "$here/../hooks.cpp" | sed '$d' > "$out/catalog_index.inc"
g++ -std=c++20 -O1 -g -Wall -Wextra -I"$here/platform" -I"$here/.." \
  -I"$here/../../../core/abi" -I"$out" "$here/test_catalog_index.cpp" -o "$out/test_catalog_index" -pthread
"$out/test_catalog_index"

# KR-KZRX: the production hk_GetProcAddress (the game's I/O ring lookups are not found, so Windows 11 reads Kiln's container
# through the ReadFile + IOCP queue Kiln answers), and install_content_hooks puts it in the game's GetProcAddress import slot.
sed -n '/^FARPROC WINAPI hk_GetProcAddress(/,/^}/p' "$here/../hooks.cpp" > "$out/ioring_gate.inc"
[ -s "$out/ioring_gate.inc" ] || { echo "FAIL ioring.hook: hooks.cpp has no hk_GetProcAddress"; exit 1; }
grep -qF 'patch_iat("KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&hk_GetProcAddress))' "$here/../hooks.cpp" ||
  { echo "FAIL ioring.install: install_content_hooks does not patch the game's GetProcAddress import"; exit 1; }
echo "PASS ioring.install: install_content_hooks patches the game's GetProcAddress import"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/.." -I"$out" \
  "$here/test_ioring_gate.cpp" -o "$out/test_ioring_gate"
"$out/test_ioring_gate"

g++ -std=c++20 -O1 -g -Wall -Wextra -DKILN_ASSETS_CATALOG_TEST -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/platform" -I"$here/.." -I"$here/../../../core/abi" -I"$here/../../../third_party" \
  "$here/test_store.cpp" "$here/../store.cpp" "$here/../catalog.cpp" "$here/../util.cpp" -o "$out/test_store" -pthread
# Resource bytes are never freed by design (the game keeps pointers into them): no leak check.
ASAN_OPTIONS=detect_leaks=0 "$out/test_store"

KILN_OUT="$KILN_OUT" bash "$here/run_vfx.sh"

# The loaded-resource reads and the scalar compare-exchange (loaded.cpp: loaded_snapshot, loaded_list,
# loaded_scalar_compare_exchange) over a fake engine, gate and guard: ASan+UBSan for every case, TSan for readers on four
# threads beside a writer that changes the fake engine's index and bytes, and for four compare-exchange writers.
loaded_flags=(-std=c++20 -O1 -g -Wall -Wextra -I"$here/.." -I"$here/../../../core/abi" -I"$here/../../engine/include")
g++ "${loaded_flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  "$here/test_loaded.cpp" "$here/../loaded.cpp" -o "$out/test_loaded" -pthread
"$out/test_loaded"
g++ "${loaded_flags[@]}" -fsanitize=thread "$here/test_loaded.cpp" "$here/../loaded.cpp" -o "$out/test_loaded_tsan" -pthread
TSAN_OPTIONS=halt_on_error=1 "$out/test_loaded_tsan" --concurrent

# Which cooked files a game loads (cooktrust.cpp): SHA-256, the cook receipts, kiln.json's "cooked".
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/.." -I"$here/../../../third_party" \
  "$here/test_cooktrust.cpp" "$here/../cooktrust.cpp" -o "$out/test_cooktrust"
"$out/test_cooktrust" "$out/cooktrust-scratch"

# A game texture's pixels as RGBA8 (texture_rgba.h, kiln_assets.h texture_rgba): resource layouts, the mip level, decoding.
# The vendored decoders and encoders are built once, without warnings (they are upstream code).
bc7=$here/../../../third_party/bc7enc
for f in bc7decomp rgbcx bc7enc; do
  [[ $out/$f.o -nt $bc7/$f.cpp ]] || g++ -std=c++20 -O1 -g -w -c "$bc7/$f.cpp" -o "$out/$f.o"
done
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/.." -I"$here/../../../third_party" \
  "$here/test_texture_rgba.cpp" "$out/bc7decomp.o" "$out/rgbcx.o" "$out/bc7enc.o" -o "$out/test_texture_rgba"
"$out/test_texture_rgba"

# Reading a mod's files from its own folder, the portable half (scan.cpp): the identity scan, the alias verdict, the
# by-name check of the assets folder and below.
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/.." -I"$here/../../../third_party" \
  "$here/test_scan.cpp" "$here/../scan.cpp" "$here/../cooktrust.cpp" -o "$out/test_scan"
"$out/test_scan" "$out/scan-scratch"
