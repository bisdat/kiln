#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# resource patches in a kiln-backend start, checked against the committed archive generation through
# kiln.boot's committed view over a fake kiln.eml.archive (bootview_eml_test.cpp), on the host, with ASan/UBSan and TSan.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests
mkdir -p "$out"
for san in address,undefined thread; do
    bin=$out/bootview-eml-test-${san%%,*}
    g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -D__fastcall= -pthread \
        -ffunction-sections -fdata-sections -fsanitize=$san -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party" \
        "$here/bootview_eml_test.cpp" "$here/../gear.cpp" -Wl,--gc-sections -o "$bin"
    "$bin"
done
