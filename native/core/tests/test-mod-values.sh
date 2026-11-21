#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln.mod-values and the Kiln-internal kiln.internal.mod-values.backend (src/services/mod_values.cpp)
# against the real owner registry and a stub backend, with ASan/UBSan; a C11 and C++20 compile of the public header and
# a C++20 compile of the internal one (src/mod_values_host.h; their static asserts), and no host-only type in core/abi:
#   native/core/tests/test-mod-values.sh
# Build output: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
cc=${CC:-$(command -v clang || command -v gcc)}
printf '#include "services/kiln_mod_values.h"\nint kiln_mod_values_header_ok;\n' |
  "$cc" -std=c11 -Wall -Wextra -Werror -pedantic -fsyntax-only -I"$here/../abi" -x c -
printf '#include "services/kiln_mod_values.h"\nint kiln_mod_values_header_ok;\n' |
  "$cxx" -std=c++20 -Wall -Wextra -Werror -pedantic -fsyntax-only -I"$here/../abi" -x c++ -
echo "kiln_mod_values.h: compiles as C11 and C++20 (layout asserts hold)"
if grep -nE 'KilnModValuesBackend|KilnModValueExport|KMV_WANT_|MOD_VALUES_BACKEND' "$here/../abi/services/kiln_mod_values.h"; then
  echo "FAIL kiln_mod_values.h holds the host-only backend table (it belongs in src/mod_values_host.h)"
  exit 1
fi
printf '#include "mod_values_host.h"\nint kiln_mod_values_host_ok;\n' |
  "$cxx" -std=c++20 -Wall -Wextra -Werror -pedantic -fsyntax-only -I"$here/../abi" -I"$here/../src" -x c++ -
echo "mod_values_host.h (Kiln-internal): compiles as C++20 (layout asserts hold); core/abi has no host-only type"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/stubs" -I"$here/../abi" -I"$here/../src" -I"$here/../../third_party" \
    -o "$out/mod_values_test" "$here/mod_values_test.cpp"
"$out/mod_values_test"
