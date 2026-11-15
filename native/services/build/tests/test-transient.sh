#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of kiln.build's transient journal (transient_journal.cpp): a store that fails a write, a rename or an append
# (or tears one), and put-back commits that fail. Build output: $KILN_OUT/build-tests (KILN_OUT default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}/build-tests
mkdir -p "$out"
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer ${KILN_TEST_DEFINES:-} \
    -o "$out/transient_journal_test" "$here/transient_journal_test.cpp" "$here/../transient_journal.cpp"
"$out/transient_journal_test"
# The region lifecycle regression runs through this existing build-service gate entry point too.
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer ${KILN_TEST_DEFINES:-} \
    -o "$out/regions_test" "$here/regions_test.cpp"
"$out/regions_test"
