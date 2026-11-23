#!/usr/bin/env bash
# Host tests of kiln-emf's EmfApi v10 wheel, v18 attributes, v22-v26 logic, v28-v47 answers and v31 on_hit (src/v26_model.*, api_*.cpp
# against fake Kiln services): no game, no Wine.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/emf-host-test
mkdir -p "$out"
g++ -std=c++20 -Wall -Wextra -I"$here/../include" "$here/abi_test.cpp" -o "$out/abi_test"
g++ -std=c++20 -Wall -Wextra -I"$here/../src" "$here/compat_ticks_test.cpp" -o "$out/compat_ticks_test"
"$out/compat_ticks_test"
"$out/abi_test"
g++ -std=c++20 -Wall -Wextra -I"$here/host" -I"$here/../include" "$here/client_probe_test.cpp" -o "$out/client_probe_test"
"$out/client_probe_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../include" -I"$here/../src" \
    "$here/v26_model_test.cpp" "$here/../src/v26_model.cpp" -o "$out/v26_model_test"
"$out/v26_model_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" \
    "$here/glue_test.cpp" "$here/../src/api_windows.cpp" "$here/../src/api_wheel.cpp" "$here/../src/api_attributes.cpp" "$here/../src/api_level.cpp" "$here/../src/api_components.cpp" "$here/../src/api_events.cpp" "$here/../src/api_v40.cpp" "$here/../src/api_terrain.cpp" "$here/../src/v26_model.cpp" \
    -o "$out/glue_test"
"$out/glue_test" "$out"
"$out/glue_test" "$out" no-render
# v44/v45/v47 terrain translation, async queue and callback timing.
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" \
    "$here/terrain_test.cpp" "$here/../src/api_terrain.cpp" -o "$out/terrain_test"
"$out/terrain_test"
# Spawn-time yaw and component forwarding use the production adapter (no game).
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" \
    "$here/spawn_test.cpp" "$here/../src/api_spawn.cpp" -o "$out/spawn_test"
"$out/spawn_test"
# v31 on_hit through the production event relay (0.2.0-dev.30.3)
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" \
    "$here/hits_test.cpp" "$here/../src/api_hits.cpp" "$here/../src/hits_model.cpp" "$here/../src/api_events.cpp" \
    -o "$out/hits_test"
for mode in "" no-mods no-relay mod-event bad-layout bad-size; do "$out/hits_test" $mode; done
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread -DKEMF_NET_CLIENT_TEST \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" \
    "$here/net_test.cpp" "$here/../src/api_net.cpp" "$here/../src/api_net_client.cpp" \
    "$here/../src/net/net_service.cpp" "$here/../src/net/net_link.cpp" "$here/../src/net/net_line.cpp" "$here/../src/net/net_peers.cpp" \
    -o "$out/net_test"
"$out/net_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread -DKEMF_PORTRAIT_TEST \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" -I"$here/../../../third_party" \
    "$here/portraits_test.cpp" "$here/../src/api_portraits.cpp" "$here/../src/v26_model.cpp" -o "$out/portraits_test"
"$out/portraits_test" "$out"
# kiln-emf's disk thread and the slow-Pre line (0.2.0-dev.29.17); argv[1]: the sources the no-disk-on-tick check reads
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread -I"$here/../src" \
    "$here/io_worker_test.cpp" -o "$out/io_worker_test"
"$out/io_worker_test" "$here/../src"
# for_each_entity's entity-map read and the per-mod cost line (0.2.0-dev.30.7); the benchmark at -O2 without sanitizers
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../src" \
    "$here/entity_scan_test.cpp" -o "$out/entity_scan_test"
"$out/entity_scan_test"
g++ -std=c++20 -O2 -Wall -Wextra -I"$here/../src" "$here/entity_scan_test.cpp" -o "$out/entity_scan_bench"
"$out/entity_scan_bench" bench
"$out/entity_scan_bench" bench fresh
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" \
    "$here/mod_cost_test.cpp" -o "$out/mod_cost_test"
"$out/mod_cost_test"
