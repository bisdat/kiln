#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The resource patches' prepare with the claim index (registry P1 S10) against its version before (commit b32bcf2d, the last
# with the full scans): 6,000 generated boot generations through both must give the same outcome, byte for byte (accepted
# with the same counts, or refused naming the same mod, key and text); then one prepare of 4,096 changes timed through both
# at -O2. Needs git (the old file comes from the repository's history). Host, ASan/UBSan for the equivalence.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to the build directory}"
out=$KILN_OUT/content-tests/bootpatch-equiv
mkdir -p "$out"
echo "== bootpatch-equiv-test (host)"
git -C "$here" show b32bcf2d:native/services/content/gamedata_bootpatch.cpp > "$out/bootpatch_old.cpp"
inc=(-I"$here/.." -I"$here/host" -I"$here/../../../core/abi" -I"$here/../../../third_party")
for v in old new; do
  src=$out/bootpatch_old.cpp; [[ $v == new ]] && src=$here/../gamedata_bootpatch.cpp
  g++ -std=c++20 -O1 -g -w -D__fastcall= -ffunction-sections -fdata-sections -fsanitize=address,undefined "${inc[@]}" -DBOOTPATCH_SRC="\"$src\"" \
      "$here/bootpatch_equiv_test.cpp" "$here/../gear.cpp" "$here/../registry/claims.cpp" -Wl,--gc-sections -o "$out/equiv-$v"
  g++ -std=c++20 -O2 -w -D__fastcall= "${inc[@]}" -DBOOTPATCH_SRC="\"$src\"" \
      "$here/bootpatch_equiv_test.cpp" "$here/../gear.cpp" "$here/../registry/claims.cpp" -o "$out/bench-$v"
  "$out/equiv-$v" > "$out/outcomes-$v.txt"
done
refused=$(grep -c " NO " "$out/outcomes-new.txt" || true)
if ! diff -q "$out/outcomes-old.txt" "$out/outcomes-new.txt" >/dev/null; then
  diff "$out/outcomes-old.txt" "$out/outcomes-new.txt" | head -20
  echo "bootpatch-equiv-test: FAIL: the outcomes differ"; exit 1
fi
echo "bootpatch-equiv-test: 6000 generations, the same outcome from both ($refused refused)"
printf 'old: '; "$out/bench-old" bench
printf 'new: '; "$out/bench-new" bench
