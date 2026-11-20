#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln.ui.snapshot (src/services/ui_snapshot.cpp) against stub worlds, with ASan/UBSan, and a C11 and C++20
# compile of its public header (its layout asserts):
#   native/core/tests/test-ui-snapshot.sh
# Build output: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
cc=${CC:-$(command -v clang || command -v gcc)}
printf '#include "services/kiln_ui_snapshot.h"\nint kiln_ui_snapshot_header_ok;\n' |
  "$cc" -std=c11 -Wall -Wextra -Werror -pedantic -fsyntax-only -I"$here/../abi" -x c -
printf '#include "services/kiln_ui_snapshot.h"\nint kiln_ui_snapshot_header_ok;\n' |
  "$cxx" -std=c++20 -Wall -Wextra -Werror -pedantic -fsyntax-only -I"$here/../abi" -x c++ -
echo "kiln_ui_snapshot.h: compiles as C11 and C++20 (layout asserts hold)"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/stubs" -I"$here/../abi" -I"$here/../src/services" \
    -o "$out/ui_snapshot_test" "$here/ui_snapshot_test.cpp"
timeout 300 "$out/ui_snapshot_test"
