#!/usr/bin/env bash
# Cooks ModelPhysics: the banner (assets/cook.json + assets/physics.json) and the pendant body's model and skeleton (blocks/pendant.json) into
# assets/cooked/ (gitignored). The installed game is only read.
#   GAME  game folder (default: Steam's Enshrouded)   EXE  its exe (default GAME/enshrouded.exe; a copy is fine)
#   KILN_OUT  tools' cache (default <repo>/.out/models)   --sources  write models/*.gltf again (make_sources.py)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
game=${GAME:-$HOME/.local/share/Steam/steamapps/common/Enshrouded}
exe=${EXE:-$game/enshrouded.exe}
export KILN_OUT=${KILN_OUT:-$root/.out/models}
if [ "${1:-}" = "--sources" ]; then
  python3 "$here/make_sources.py" "$here/models" > /dev/null
  rm -f "$here/models/cape.gltf"   # the cape is the client fixture's (mods/tests/model-physics-client)
fi
python3 "$root/tools/modelcook/modelcook.py" --game "$game" --exe "$exe" recipe "$here/assets/cook.json" --update-manifest
