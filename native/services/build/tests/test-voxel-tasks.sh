#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln.build's task-thread voxel grid (voxel_tasks.cpp, voxel_tasks_hold.cpp) over the real kiln.engine (its
# views, write gate and guarded copies, booted with a fake core) and a fake engine from both exact profiles, under
# ASan/UBSan and ThreadSanitizer. Build output: $KILN_OUT/build-tasks-test (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/build-tasks-test
mkdir -p "$out"
cxx=${CXX:-g++}
engine="$here/../../engine"
for kind in asan tsan; do
  if [[ $kind == asan ]]; then san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined); else san=(-fsanitize=thread); fi
  "$cxx" -std=c++20 -O1 -g -pthread -Wall -Wextra -Werror -Wno-mismatched-new-delete "${san[@]}" -DKILN_ENGINE_GATE_TEST_POINTS \
      -DKILN_BUILD_HOLD_TEST_POINTS -I"$engine" -I"$engine/include" -I"$engine/tests" -I"$here/../../../core/abi" \
      "$here/voxel_tasks_test.cpp" "$here/../voxel_tasks.cpp" "$here/../voxel_tasks_hold.cpp" \
      "$engine/engine.cpp" "$engine/identity.cpp" "$engine/resolve.cpp" "$engine/gate.cpp" "$engine/resource_guard.cpp" \
      "$engine/invoke.cpp" "$engine/tests/host/memory_host.cpp" "$engine/tests/host/invoke_host.cpp" \
      -o "$out/voxel_tasks_test_$kind"
  TSAN_OPTIONS=halt_on_error=1:exitcode=66 "$out/voxel_tasks_test_$kind"
done
