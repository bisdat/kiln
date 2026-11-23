#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through the project's build limiter. No game execution; binaries go in the caller's scratch directory.
set -euo pipefail
out=${1:?usage: test-terrain-carve.sh output-directory}
root=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$out"
${CXX:-c++} -std=c++20 -Wall -Wextra -Werror -I "$root/core/abi" \
    "$root/services/tools/tests/terrain_carve_test.cpp" -o "$out/terrain-carve-test"
"$out/terrain-carve-test"
${CXX:-c++} -std=c++20 -Wall -Wextra -Werror -pthread -I "$root/core/abi" \
    "$root/services/tools/tests/terrain_carve_hooks_test.cpp" -o "$out/terrain-carve-hooks-test"
for scenario in client server bad-kernel bad-buildings missing-kernel missing-buildings unknown-build unverified safe-idle no-hooks refuse-kernel refuse-buildings retry-buildings managed-code foreign-kernel foreign-buildings unknown-code state-error original-error no-original short-hooks no-code-state owner-failure parent-owner no-owner-create no-owner-revoke no-revoke-owner bad-dispatch missing-dispatch refuse-dispatch foreign-dispatch no-report-layout bad-report-layout; do
    "$out/terrain-carve-hooks-test" "$scenario"
done
