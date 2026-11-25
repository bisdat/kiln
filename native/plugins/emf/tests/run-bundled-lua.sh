#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Boot the production plugin and the verified upstream DLL under Wine, with a fake core and no game.
# Build kiln-emf and emf-bundled-lua-test into KILN_OUT first. KILN_EMF_LUA_ARCHIVE may point to the pinned ZIP.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
bin=$KILN_OUT/cmake/bin
work=$(mktemp -d "$KILN_OUT/emf-lua-test.XXXXXX")
trap 'echo "EMF Lua fixture files: $work"' EXIT
export WINEPREFIX=$work/wine WINEDEBUG=-all
python3 -I "$repo/tools/package/stage-emf-lua.py" "$work/payload"

fixture() { # name mode expected [script API min] [host range] [script side]
  local name=$1 mode=$2 expected=$3 api=${4:-14} range=${5:->=1.25} side=${6:-server}
  local root=$work/$name
  mkdir -p "$root/kiln/"{plugins,config/emf,logs,state}
  cp "$bin/emf-test/emf-bundled-lua-test.exe" "$root/"
  cp "$bin/plugins/kiln-emf.dll" "$root/kiln/plugins/"
  if [[ $name != missing ]]; then cp -a "$work/payload" "$root/kiln/plugins/emf-lua"; fi
  if [[ $name != unused ]]; then
    mkdir -p "$root/emf/mods/lua-smoke"
    printf '{"id":"lua-smoke","version":"1.0.0","main":"main.lua","emf":%s,"side":"%s","depends":{"lua":"%s"}}\n' \
      "$api" "$side" "$range" > "$root/emf/mods/lua-smoke/mod.json"
    printf 'emf.log("bundled-lua-smoke-ran")\n' > "$root/emf/mods/lua-smoke/main.lua"
  fi
  case $name in
    explicit-off)
      mkdir -p "$root/emf/mods/lua"
      cp "$work/payload/lua.dll" "$root/emf/mods/lua/"
      printf '{"id":"lua","version":"1.43.0","emf":20,"enabled":false}\n' > "$root/emf/mods/lua/mod.json" ;;
    bundle-off)
      printf '[lua]\nenabled=0\n' > "$root/kiln/config/emf/lua.ini" ;;
    invalid-bundle)
      printf '{"id":"lua","version":"1.43.0","emf":20,"dll":"wrong.dll"}\n' > "$root/kiln/plugins/emf-lua/mod.json" ;;
    own-emf)
      cp "$bin/plugins/kiln-emf.dll" "$root/emf_core.dll" ;; # mapped marker only; no EMF engine or code executed
  esac
  if ! timeout 90s wine "$root/emf-bundled-lua-test.exe" "$mode" "$expected" > "$root/run.log" 2>&1; then
    cat "$root/run.log" >&2
    return 1
  fi
  printf 'PASS: %s\n' "$name"
}

fixture unused legacy ignored
fixture drop-in legacy runs
fixture profile profile runs
fixture dedicated dedicated runs
fixture no-mods no-mods ignored
fixture invalid-profile invalid ignored
fixture own-emf own-emf ignored
fixture explicit-off legacy ignored
fixture bundle-off legacy ignored
fixture invalid-bundle legacy ignored
fixture missing legacy ignored
fixture api-too-high legacy ignored 50
fixture version-too-new legacy ignored 14 '>=1.44'
fixture client-only dedicated ignored 14 '>=1.25' client
