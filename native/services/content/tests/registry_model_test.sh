#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.content registry stage: the model (registry/model.cpp) over a fake tree view and over hand-made bytes and types, on the
# host, with ASan/UBSan.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
echo "== registry-model-test (host)"
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -D__fastcall= -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
    "$here/registry_model_test.cpp" "$here/registry_model_fakes.cpp" "$here/../registry/model.cpp" "$here/../registry/regmap.cpp" "$here/../registry/ir.cpp" \
    "$here/../registry/compose.cpp" "$here/../registry/formulas.cpp" "$here/../gear.cpp" -Wl,--gc-sections -o "$out/registry-model-test"
"$out/registry-model-test" "$here/../../../../regmap"
