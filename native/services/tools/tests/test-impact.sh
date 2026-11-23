#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through the shared build limiter. Native Windows uses the same sources with MSVC /EHa.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: test-impact.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
abi="$here/../../../core/abi"
cxx=${CXX:-c++}
"$cxx" -std=c++20 -D__fastcall= -Wall -Wextra -Werror -I"$abi" \
    "$here/impact_args_test.cpp" -o "$out/impact-args-test"
"$out/impact-args-test"
"$cxx" -std=c++20 -Wall -Wextra -Werror -pthread -I"$abi" "$here/action_trace.cpp" -o "$out/action-trace-test"
"$out/action-trace-test"
"$cxx" -std=c++20 -D__fastcall= -Wall -Wextra -Werror -pthread -I"$abi" \
    "$here/impact_dispatch_test.cpp" "$here/../impact_binding.cpp" "$here/../policy.cpp" -o "$out/impact-dispatch-test"
"$out/impact-dispatch-test"
for test in bootstrap capabilities; do
    "$cxx" -std=c++20 -Wall -Wextra -Werror -I"$abi" "$here/${test}_test.cpp" -o "$out/${test}-test"
    "$out/${test}-test"
done
bash "$here/test-service-presentation.sh" "$out"
