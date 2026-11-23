#!/usr/bin/env bash
# kiln.camera without a game or Wine: the header as C (its layout and numbers), the lease model on a fake backend (who
# holds an endpoint, the order of answers and events, owners and worlds that go), the snapshot and the render
# transaction on a fake renderer, the service's server and client entry points, and the render path's threads (TSan).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
abi=$here/../../../core/abi
: "${KILN_OUT:?set KILN_OUT to a dedicated build directory}"
out=$KILN_OUT/camera-tests
mkdir -p "$out"
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I"$abi" "$here/abi_test.c" -o "$out/abi-test"
"$out/abi-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/state_test.cpp" "$here/../state.cpp" -o "$out/state-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/mode_test.cpp" "$here/../mode.cpp" "$here/../state.cpp" -o "$out/mode-test"
"$out/mode-test"
"$out/state-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/capture_test.cpp" "$here/../capture.cpp" -o "$out/capture-test"
"$out/capture-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/render_test.cpp" "$here/../render_tx.cpp" "$here/../sampler.cpp" "$here/../capture.cpp" "$here/../state.cpp" -o "$out/render-test"
"$out/render-test"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/service_test.cpp" "$here/../camera.cpp" "$here/../mode.cpp" "$here/../state.cpp" "$here/../capture.cpp" "$here/../local.cpp" "$here/../sampler.cpp" "$here/../render_tx.cpp" "$here/../bridge.cpp" -o "$out/service-test"
"$out/service-test"
"$out/service-test" no-observer
"$out/service-test" old-world
"$out/service-test" declare-refused
"$out/service-test" program-off
"$out/service-test" bridge
# across threads: the owner-retiring barrier, and world stops and retirements against the tick thread (TSan, then ASan)
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=thread -pthread \
    -I"$abi" "$here/threads_test.cpp" "$here/../mode.cpp" "$here/../state.cpp" -o "$out/threads-tsan"
"$out/threads-tsan" 3000
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/threads_test.cpp" "$here/../mode.cpp" "$here/../state.cpp" -o "$out/threads-asan"
"$out/threads-asan" 6000
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$abi" "$here/client_test.cpp" "$here/../camera.cpp" "$here/../mode.cpp" "$here/../state.cpp" "$here/../capture.cpp" "$here/../local.cpp" "$here/../sampler.cpp" "$here/../render_tx.cpp" "$here/../bridge.cpp" -o "$out/client-test"
for scenario in ok no-constants no-capture no-mid2 bad-reflection pose-row-off sampler-fault; do "$out/client-test" "$scenario"; done
# the render path and the client Pre on their own threads, under ThreadSanitizer (which warns that it does not model
# the seqlocks' fences; a race it does see still fails the run)
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-tsan -fsanitize=thread -pthread \
    -I"$abi" "$here/concurrency_test.cpp" "$here/../render_tx.cpp" "$here/../sampler.cpp" "$here/../capture.cpp" "$here/../state.cpp" -o "$out/concurrency-test"
"$out/concurrency-test"
# S8: the transport binding and the REMOTE_VISUAL backend (ASan/UBSan, then TSan)
bash "$here/run-bridge.sh"
