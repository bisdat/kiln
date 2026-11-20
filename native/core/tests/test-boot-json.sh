#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of state/boot.json's text and kiln.json "unloadAtExit"'s text in the boot log. Cuts the pure block between
# "boot-json:begin" and "boot-json:end" out of native/core/src/boot.cpp as it is, builds boot_json_test.cpp around it
# with the host's C++ compiler and runs it. Exit code 0 = every check passed. No Wine and no game are needed.
#   native/core/tests/test-boot-json.sh
# Env: KILN_OUT               build dir (default native/build); the test goes to $KILN_OUT/boot-json-test
#      KILN_BOOT_JSON_SRC     another boot.cpp to cut the block from (to see a broken rule fail)
#      CXX                    the host compiler (default c++)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=${KILN_BOOT_JSON_SRC:-$here/../src/boot.cpp}
out=${KILN_OUT:-$here/../../build}/boot-json-test
mkdir -p "$out"
sed -n '/^\/\/ boot-json:begin$/,/^\/\/ boot-json:end$/p' "$src" >"$out/boot_json_block.inc"
[[ -s $out/boot_json_block.inc ]] || { echo "no boot-json block in $src" >&2; exit 2; }
"${CXX:-c++}" -std=c++20 -O1 -Wall -Wextra -Werror -I "$out" -I "$here/../../third_party" "$here/boot_json_test.cpp" \
    -o "$out/boot_json_test"
"$out/boot_json_test"
