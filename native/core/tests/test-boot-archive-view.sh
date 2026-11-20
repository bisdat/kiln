#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of the base view (services/boot_archive_view.h) and of kiln.boot reading through it
# (services/boot_service.cpp's base policy), each built twice: with ASan/UBSan and with TSan.
#   native/core/tests/test-boot-archive-view.sh
# Build output and scratch: $KILN_OUT/core-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
flags=(-std=c++20 -O1 -g0 -Wall -Wextra -Wno-unused-function -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections
       -I"$here/stubs" -I"$here/../abi" -I"$here/../src" -I"$here/../../third_party")
asan=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
tsan=(-fsanitize=thread)
rc=0
for t in boot_archive_view_test boot_base_service_test; do
  "$cxx" "${flags[@]}" "${asan[@]}" -o "$out/$t-asan" "$here/$t.cpp"
  "$cxx" "${flags[@]}" "${tsan[@]}" -o "$out/$t-tsan" "$here/$t.cpp"
  for s in asan tsan; do
    echo "== $t ($s)"
    mkdir -p "$out/$s"
    TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" timeout 300 "$out/$t-$s" "$out/$s" || rc=1
  done
done
exit $rc
