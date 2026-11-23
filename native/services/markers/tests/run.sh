#!/usr/bin/env bash
# kiln.markers' host tests: no game, no Wine. The v1 ABI (sizes and offsets; the header compiles as C11 and C++), the store, the icon catalog,
# wire v1 and the link.
#   KILN_OUT=<build dir> native/services/markers/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
abi=$here/../../../core/abi
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/markers-host-test
mkdir -p "$out"

# The header is C: it compiles as C11 (its own size asserts included).
printf '#include "services/kiln_markers.h"\n_Static_assert(KILN_MARKER_WORLD == 4, "surfaces");\n' |
  gcc -std=c11 -Wall -Wextra -pedantic -fsyntax-only -I"$abi" -x c -
echo "PASS kiln_markers.h compiles as C11"
printf '#include "services/kiln_markers_lab.h"\n_Static_assert(sizeof(KilnMarkersLabApi) == 32, "lab table");\n' |
  gcc -std=c11 -Wall -Wextra -pedantic -fsyntax-only -I"$abi" -x c -
echo "PASS kiln_markers_lab.h compiles as C11"

g++ -std=c++20 -O1 -g -Wall -Wextra -I"$abi" "$here/abi_test.cpp" -o "$out/abi_test"
"$out/abi_test"

# The logical store with a fake world clock (M01, M02, M14 logical half), its publishers, limits, error rule, world stop
# and subscriptions; four writer threads against a dispatching one.
src=$here/..
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$src" -I"$abi" \
  "$here/store_test.cpp" "$src/store.cpp" "$src/events.cpp" "$src/arena.cpp" "$src/text.cpp" "$src/guard.cpp" -o "$out/store_test"
"$out/store_test"

# The icon catalog on synthetic resources (M13 client half): readers and bounds, keys and aliases, the digest, resolve,
# warm-up, the fast-travel refusal, the build pair. (tests/catalog_offline.cpp reads a client's dumped resources; it is
# not run here: it needs the game's files.)
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$src" -I"$abi" \
  "$here/catalog_test.cpp" "$src/catalog.cpp" "$src/store.cpp" "$src/events.cpp" "$src/arena.cpp" "$src/text.cpp" "$src/guard.cpp" \
  -o "$out/catalog_test"
"$out/catalog_test"

# Admission, visibility and the native map list on fakes (M03 with a sentinel list, M04, M14 visibility half, the reason
# precedence, the entry encoder, the heading).
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$src" -I"$abi" \
  "$here/surfaces_test.cpp" "$src/select.cpp" "$src/visibility.cpp" "$src/map_list.cpp" -o "$out/surfaces_test"
"$out/surfaces_test"

# S3, following an entity (M05/M06 mechanics): the watch table on a fake world and storage (serials, leases, same-id
# recreation, a missing transform and its recovery, destroy terminal, quotas, epochs, world stop), the explicit-world reader
# and its layout check against a fake kiln.types, the local store following an entity, and the observers on four threads
# against a Post reader and a capturer.
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$src" -I"$abi" \
  "$here/anchors_test.cpp" "$src/watch.cpp" "$src/world_read.cpp" "$src/store.cpp" "$src/events.cpp" "$src/arena.cpp" "$src/text.cpp" \
  "$src/guard.cpp" -o "$out/anchors_test"
"$out/anchors_test"

# Wire v1 (S7): byte-exact golden frames of the eleven messages and a RECORD, strict reads, a record's meaning.
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$src" -I"$abi" \
  "$here/wire_test.cpp" "$src/wire.cpp" "$src/text.cpp" -o "$out/wire_test"
"$out/wire_test"

# The link (S7, S8 native): the server layers and the server role against built-in receivers over a fake bridge, network
# and clock: M09, M10, M11, M13 server half, the hosted loopback, follow poses, limits, the error rule, the bridge table.
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$src" -I"$abi" \
  "$here/link_test.cpp" "$src/server.cpp" "$src/receiver.cpp" "$src/bridge.cpp" "$src/wire.cpp" "$src/catalog.cpp" "$src/store.cpp" \
  "$src/events.cpp" "$src/arena.cpp" "$src/text.cpp" "$src/guard.cpp" -o "$out/link_test"
"$out/link_test"

# The glue (markers.cpp) through the core's start, kiln_plugin_main then on_ready, on a fake core for a dedicated server
# and a game client (joined or hosting): on_ready returns on each, and only a client registers for the game's resources.
# Anchors and surfaces are stand-ins (game hooks, render); the link is real. One child process per case.
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$here/stubs" -I"$src" \
  -I"$abi" "$here/ready_test.cpp" "$src/markers.cpp" "$src/link.cpp" "$src/server.cpp" "$src/receiver.cpp" "$src/bridge.cpp" \
  "$src/wire.cpp" "$src/catalog.cpp" "$src/store.cpp" "$src/events.cpp" "$src/arena.cpp" "$src/text.cpp" "$src/guard.cpp" \
  -o "$out/ready_test"
"$out/ready_test"
