#!/usr/bin/env bash
# Host tests of kiln-emf's EmfApi v10 wheel, v18 attributes, v22-v26 logic, v28-v46 answers and v31 on_hit (src/v26_model.*, api_*.cpp
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
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../include" -I"$here/../src" \
    "$here/v26_model_test.cpp" "$here/../src/v26_model.cpp" -o "$out/v26_model_test"
"$out/v26_model_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread \
    -I"$here/host" -I"$here/../include" -I"$here/../src" -I"$here/../../../core/abi" -I"$here/../../../services/players" \
    "$here/glue_test.cpp" "$here/../src/api_windows.cpp" "$here/../src/api_wheel.cpp" "$here/../src/api_attributes.cpp" "$here/../src/api_level.cpp" "$here/../src/api_components.cpp" "$here/../src/api_events.cpp" "$here/../src/api_v40.cpp" "$here/../src/v26_model.cpp" \
    -o "$out/glue_test"
"$out/glue_test" "$out"
"$out/glue_test" "$out" no-render
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
