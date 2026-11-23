#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Linux policy/witness tests, ASan + UBSan. Invoke through the project's host build slot.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${1:?usage: storage-host.sh output-directory}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$root/native/core/abi" -I "$root/native/third_party" "$here/storage_logic_test.cpp" -o "$out/storage-logic-test"
"$out/storage-logic-test" "$root/tests/fixtures/player-storage/witness-vectors.json"
