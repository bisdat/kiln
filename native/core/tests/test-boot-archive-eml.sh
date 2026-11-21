#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of kiln.boot in a kiln-backend start (services/boot_archive_eml.h, boot_service.cpp's kiln-backend base)
# against a fake kiln.eml.archive table, built with ASan/UBSan and with TSan. PRIVATE (tools/sfi/public_cut.py).
#   native/core/tests/test-boot-archive-eml.sh
# Build output and scratch: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
flags=(-std=c++20 -O1 -g0 -Wall -Wextra -Wno-unused-function -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections
       -I"$here/stubs" -I"$here/../abi" -I"$here/../src" -I"$here/../../third_party")
rc=0
"$cxx" "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined -o "$out/boot_archive_eml_test-asan" "$here/boot_archive_eml_test.cpp"
"$cxx" "${flags[@]}" -fsanitize=thread -o "$out/boot_archive_eml_test-tsan" "$here/boot_archive_eml_test.cpp"
for s in asan tsan; do
  echo "== boot_archive_eml_test ($s)"
  mkdir -p "$out/$s"
  TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" timeout 300 "$out/boot_archive_eml_test-$s" "$out/$s" || rc=1
done
exit $rc
