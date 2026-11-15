#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/gameplay-host-test
mkdir -p "$out"
g++ -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
    -I"$here/../../../core/abi" "$here/component_change_test.cpp" -o "$out/component_change_test"
"$out/component_change_test"
g++ -std=c++20 -D__fastcall= -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
    -I"$here/../../../core/abi" "$here/build_hooks_test.cpp" "$here/../demand_hooks.cpp" -o "$out/build_hooks_test"
"$out/build_hooks_test"
"$out/build_hooks_test" failure
"$out/build_hooks_test" immediate
"$out/build_hooks_test" hosted
"$out/build_hooks_test" precheck
"$out/build_hooks_test" unobserved
"$out/build_hooks_test" zones
g++ -std=c++20 -D__fastcall= -O1 -g -pthread -ffunction-sections -fdata-sections -Wall -Wextra -fsanitize=address,undefined \
    -I"$here/../../../core/abi" "$here/remaining_hooks_test.cpp" "$here/../hooks.cpp" "$here/../demand_hooks.cpp" \
    "$here/../entities.cpp" "$here/../zones.cpp" -Wl,--gc-sections -o "$out/remaining_hooks_test"
for mode in idle craft craft-threads craft-late craft-refuse craft-tick spawn spawn-threads spawn-late spawn-refuse spawn-tick spawn-replace spawn-replace-late spawn-replace-refuse spawn-zone spawn-zone-late spawn-update spawn-update-late \
    terraform terraform-subscribe terraform-tick terraform-threads terraform-late terraform-refuse terraform-zone terraform-zone-late terraform-update terraform-update-late \
    entities entities-threads entities-late entities-refuse entities-partial-refuse entities-tick entities-filter; do
    "$out/remaining_hooks_test" "$mode"
done
