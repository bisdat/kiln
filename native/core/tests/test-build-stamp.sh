#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# native/cmake/kiln_build_stamp.sh, the build metadata every build entry point writes (CMake at configure time and
# before every stamped target; native/build.sh builds through CMake): a git work tree, the KILN_GIT/SOURCE_DATE_EPOCH
# overrides, a source export without git (a nonce per build directory), an export inside another repository, a bad
# override. No build, no Wine.
#   KILN_OUT=<scratch> native/core/tests/test-build-stamp.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
script=$repo/native/cmake/kiln_build_stamp.sh
out=${KILN_OUT:-$here/../../build}/build-stamp-test
rm -rf "$out"
mkdir -p "$out"
checks=0 failures=0
check() { # condition-result description
  checks=$((checks + 1))
  if [[ $1 == 0 ]]; then printf 'ok   %s\n' "$2"; else failures=$((failures + 1)); printf 'FAIL %s\n' "$2"; fi
}
stamp_of() { sed -n 's/^#define KILN_BUILD "\(.*\)"$/\1/p' "$1/kiln_build.h"; }
# The production script's rule (kiln_build_stamp.sh): git identity only when the source root IS the work tree's top.
top=$(git -C "$repo" rev-parse --show-toplevel 2>/dev/null || true)
is_top=0
if [[ -n $top && $(realpath -- "$top") == $(realpath -- "$repo") ]]; then is_top=1; fi

# a git work tree: +<UTC commit date>.<short HEAD>, no nonce
if ((is_top)); then
  env -u SOURCE_DATE_EPOCH -u KILN_GIT bash "$script" "$repo" "$out/git"
  want="+$(date -u -d "@$(git -C "$repo" log -1 --format=%ct)" +%Y%m%d).$(git -C "$repo" rev-parse --short=7 HEAD)"
  [[ $(stamp_of "$out/git") == "$want" ]]; check $? "git work tree: $(stamp_of "$out/git")"
  [[ ! -e $out/git/kiln_build_nonce ]]; check $? "git work tree: no nonce"
fi
# a source export (no git, or only an enclosing repository): a nonce, never a borrowed commit
if ((!is_top)); then
  env -u SOURCE_DATE_EPOCH -u KILN_GIT bash "$script" "$repo" "$out/export-root"
  [[ $(stamp_of "$out/export-root") =~ ^\+nogit\.[0-9a-f]{16}$ ]]; check $? "this source export: a nonce ($(stamp_of "$out/export-root"))"
fi
# the package overrides win
SOURCE_DATE_EPOCH=86400 KILN_GIT=abc1234 bash "$script" "$repo" "$out/override"
[[ $(stamp_of "$out/override") == "+19700102.abc1234" ]]; check $? "SOURCE_DATE_EPOCH and KILN_GIT override"
# a source export without git: one nonce per build directory, kept; another build directory gets another
mkdir -p "$out/export"
env -u SOURCE_DATE_EPOCH -u KILN_GIT bash "$script" "$out/export" "$out/a"
first=$(stamp_of "$out/a")
[[ $first =~ ^\+nogit\.[0-9a-f]{16}$ ]]; check $? "no git: a nonce stamp ($first)"
before=$(stat -c %Y "$out/a/kiln_build.h")
sleep 1
env -u SOURCE_DATE_EPOCH -u KILN_GIT bash "$script" "$out/export" "$out/a"
[[ $(stamp_of "$out/a") == "$first" ]]; check $? "no git: the same build directory keeps its nonce"
after=$(stat -c %Y "$out/a/kiln_build.h")
[[ $before == "$after" ]]; check $? "an unchanged stamp is not rewritten (nothing rebuilds)"
env -u SOURCE_DATE_EPOCH -u KILN_GIT bash "$script" "$out/export" "$out/b"
[[ $(stamp_of "$out/b") =~ ^\+nogit\.[0-9a-f]{16}$ && $(stamp_of "$out/b") != "$first" ]]
check $? "no git: another build directory gets another nonce"
# an export placed inside another repository never borrows that repository's commit
if [[ -n $top ]]; then
  env -u SOURCE_DATE_EPOCH -u KILN_GIT bash "$script" "$repo/native" "$out/nested"
  [[ $(stamp_of "$out/nested") =~ ^\+nogit\. ]]; check $? "an export inside another repository: nonce, not that commit"
fi
# a bad override stops the build instead of writing a broken header
if KILN_GIT='a"b' SOURCE_DATE_EPOCH=0 bash "$script" "$repo" "$out/bad" 2>/dev/null; then check 1 "a bad KILN_GIT is refused"
else check 0 "a bad KILN_GIT is refused"; fi
[[ ! -e $out/bad/kiln_build.h ]]; check $? "and writes no header"
# the whole suite from a source export nested inside another repository (a fresh git repo here): it must expect the
# nonce the script gives there, not that repository's commit
if [[ -z ${KILN_STAMP_TEST_NESTED:-} ]] && command -v git >/dev/null; then
  parent=$out/parent
  mkdir -p "$parent/export/native/cmake" "$parent/export/native/core/tests"
  git init -q "$parent"
  cp "$script" "$parent/export/native/cmake/"
  cp "$here/test-build-stamp.sh" "$parent/export/native/core/tests/"
  if KILN_STAMP_TEST_NESTED=1 KILN_OUT=$out/nested-run bash "$parent/export/native/core/tests/test-build-stamp.sh" \
    >"$out/nested-run.log" 2>&1; then check 0 "the suite passes from an export nested inside another repository"
  else check 1 "the suite passes from an export nested inside another repository ($out/nested-run.log)"; fi
fi
printf 'build stamp: %d checks, %d failures\n' "$checks" "$failures"
((failures == 0))
