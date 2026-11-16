#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compile-only: kiln_world.h and kiln_abilities.h together in both orders (C11 and C++20), and each alone (C++):
# each header's program-info struct has its own name (KilnWorldProgramInfo, KilnImpactProgramInfo), KilnProgramInfo stays
# an alias of the first one's. Output: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cc=${CC:-$(command -v clang || command -v gcc)}
cxx=${CXX:-$(command -v clang++ || command -v g++)}
inc=(-I"$here/../abi")
n=0
for f in world_first.c abilities_first.c; do
    "$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" "$here/abi_headers/$f"; n=$((n + 1))
done
for d in "" -DKILN_TEST_ABILITIES_FIRST -DKILN_TEST_ABILITIES_ONLY -DKILN_TEST_WORLD_ONLY; do
    "$cxx" -std=c++20 -Wall -Wextra -Werror -fsyntax-only $d "${inc[@]}" "$here/abi_headers/both.cpp"; n=$((n + 1))
done
# kiln.tasks v1: every size/offset/tag as C11 and C++20, and the Kiln-internal host table (src/tasks_host.h, C++ only)
"$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" "$here/tasks_abi_test.c"; n=$((n + 1))
"$cxx" -std=c++20 -x c++ -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" "$here/tasks_abi_test.c"; n=$((n + 1))
"$cxx" -std=c++20 -x c++ -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -I"$here/../src" -include tasks_host.h /dev/null; n=$((n + 1))
echo "abi headers: PASS $n compile-only checks (kiln_world.h and kiln_abilities.h together in both orders, C and C++; each alone; kiln_tasks.h, tasks_host.h)"
