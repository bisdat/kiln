#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Writes <generated>/kiln_build.h, the build metadata of one build: the core and every kiln_service/kiln_plugin DLL
# include it (the core grants Kiln-internal access only to DLLs whose kiln_build_stamp() equals its own identity).
#   KILN_BUILD = "+<yyyymmdd>.<git>": the repository's HEAD commit (UTC commit date); SOURCE_DATE_EPOCH and KILN_GIT
#                override it, as in tools/package/package.sh. Only when <repo> is itself the top of a git work tree, so a
#                source export inside another repository never borrows that repository's commit.
#   KILN_BUILD = "+nogit.<nonce>": no git (a source export): a random nonce made once per build directory and kept in
#                <generated>/kiln_build_nonce, so one build's core and DLLs share it and another build's do not.
# Written only when it changes (an unchanged stamp rebuilds nothing). Run by native/CMakeLists.txt at configure time
# and before every build of a stamped target (native/build.sh builds through it).
#   native/cmake/kiln_build_stamp.sh <repo> <generated dir>
set -euo pipefail
repo=$1
generated=$2
mkdir -p "$generated"
top=$(git -C "$repo" rev-parse --show-toplevel 2>/dev/null || true)
epoch=${SOURCE_DATE_EPOCH:-}
git=${KILN_GIT:-}
if [[ -n $top && $(realpath -- "$top") == $(realpath -- "$repo") ]]; then
  [[ -n $epoch ]] || epoch=$(git -C "$repo" log -1 --format=%ct 2>/dev/null || true)
  [[ -n $git ]] || git=$(git -C "$repo" rev-parse --short=7 HEAD 2>/dev/null || true)
fi
if [[ -n $epoch && -n $git ]]; then
  stamp="+$(date -u -d "@$epoch" +%Y%m%d).$git"
else
  nonce=$generated/kiln_build_nonce
  if [[ ! -s $nonce ]]; then
    od -An -N8 -tx1 /dev/urandom | tr -d ' \n' > "$nonce.tmp"
    mv -f "$nonce.tmp" "$nonce"
  fi
  stamp="+nogit.$(<"$nonce")"
fi
[[ $stamp =~ ^\+[0-9A-Za-z.-]+$ ]] || { printf 'kiln_build_stamp.sh: bad build stamp %s\n' "$stamp" >&2; exit 1; }
line="#define KILN_BUILD \"$stamp\""
if [[ ! -f $generated/kiln_build.h || $(<"$generated/kiln_build.h") != "$line" ]]; then
  printf '%s\n' "$line" > "$generated/kiln_build.h.tmp"
  mv -f "$generated/kiln_build.h.tmp" "$generated/kiln_build.h"
fi
