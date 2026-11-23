#!/usr/bin/env bash
# Builds and runs kiln.worldtables' host test: ids, the handshake hash, names and the table writers, on synthetic resources. No game, no Wine.
#   native/services/worldtables/tests/run.sh [registry.bin pools.bin blueprints.bin world.bin [door-wall-model.bin [flat-roof-2m-model.bin]]]   (KILN_TERRAIN_RES: below)
# The optional files are a game's own resources (local data, never in the repo): the test then also checks that a merge with nothing to do gives
# the same bytes back, and that a new material reads back from them.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/worldtables-host-test
mkdir -p "$out"
python3 -I "$here/test_startup_sections.py"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/.." -I"$here/../../surfaces/cook" -I"$here/../../../third_party" \
    "$here/test_merge.cpp" "$here/../reflect.cpp" "$here/../merge.cpp" "$here/../terrain.cpp" "$here/../../surfaces/cook/surfacecheck.cpp" -o "$out/test_merge"
"$out/test_merge" "$@"
# Terrain materials: the same, and with KILN_TERRAIN_RES=<folder of the game's registry.bin pools.bin world.bin vfx.bin blend.bin simple.bin> it also
# merges two into them and leaves merged-*.bin there for tests/terrain_roundtrip.py.
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/.." -I"$here/../../surfaces/cook" -I"$here/../../../third_party" \
    "$here/test_terrain.cpp" "$here/../merge.cpp" "$here/../terrain.cpp" "$here/../../surfaces/cook/surfacecheck.cpp" -o "$out/test_terrain"
"$out/test_terrain" ${KILN_TERRAIN_RES:+"$KILN_TERRAIN_RES"}
# Tool-only documents need no unrelated section to register their pools and brushes.
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/.." -I"$here/../../surfaces/cook" -I"$here/../../../third_party" \
    "$here/test_tool_pools.cpp" "$here/../merge.cpp" "$here/../terrain.cpp" "$here/../../surfaces/cook/surfacecheck.cpp" -o "$out/test_tool_pools"
"$out/test_tool_pools"
# Durable next-start pools use the reviewed shared pending-store boundary.
KILN_OUT="$KILN_OUT" bash "$here/pool_store_test.sh"
