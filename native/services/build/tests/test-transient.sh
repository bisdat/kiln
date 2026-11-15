#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln.build's transient journal (transient_journal.cpp): a store that fails a write, a rename or an append
# (or tears one), and put-back commits that fail. Build output: $KILN_OUT/build-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/build-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer ${KILN_TEST_DEFINES:-} \
    -o "$out/transient_journal_test" "$here/transient_journal_test.cpp" "$here/../transient_journal.cpp"
"$out/transient_journal_test"
# The region lifecycle regression runs through this existing build-service gate entry point too.
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer ${KILN_TEST_DEFINES:-} \
    -o "$out/regions_test" "$here/regions_test.cpp"
"$out/regions_test"

# Actual voxel cache with the engine resolution adapter replaced.
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$out/voxel_lifecycle_test" "$here/voxel_lifecycle_test.cpp"
"$out/voxel_lifecycle_test"

# World-state admission and the save-file proof on which automatic terrain installation relies.
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$out/world_state_test" "$here/world_state_test.cpp"
"$out/world_state_test"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$out/new_world_test" "$here/../../saves/tests/new_world_test.cpp"
"$out/new_world_test"

"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$out/terrain_budget_test" "$here/terrain_budget_test.cpp"
"$out/terrain_budget_test"
