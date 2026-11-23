#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through the shared build slot; pass an output directory outside the source tree.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: test-policy.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pthread -I"$here/.." \
    "$here/policy_test.cpp" "$here/../policy.cpp" -o "$out/policy-test"
"$out/policy-test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -I"$here/../include" \
    "$here/policy_header.c" -o "$out/policy-header-test"
"$out/policy-header-test"
echo "tools policy C header: 1 test passed"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I"$here/../../../core/abi" \
    "$here/abi.cpp" -o "$out/abi-test"
"$out/abi-test"
echo "tools public ABI: 1 test passed"
