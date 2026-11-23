#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.staged on the host (no Windows, no game): src/staged.cpp as it is, with ASan+UBSan, every crash point of five
# scenarios run in forked children (native/core/tests/staged_test.cpp). The Win32 file calls: test-staged-wine.sh.
#   native/core/tests/test-staged.sh
# Environment: KILN_OUT (output folder, default native/build), CXX (default clang++).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
out=${KILN_OUT:-$here/../../build}/staged-test
mkdir -p "$out"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/kiln-staged-test.XXXXXX")
trap 'chmod -R u+w "$scratch" 2>/dev/null; rm -rf "$scratch"' EXIT
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$src" -I"$here/../abi" -I"$here/../../third_party" "$here/staged_test.cpp" "$src/staged.cpp" -o "$out/staged_test"
KILN_STAGED_SCRATCH=$scratch/run KILN_STAGED_FIXTURE=$here/../../../schema/fixtures/staged/paths.json "$out/staged_test"
