#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The core's build-stamp test against a staged runtime (sfi_stamp_win.cpp: kiln-sfi.dll is the core's build, so the core
# makes it the ShroudForge-dialect owner) under headless Wine. On Windows, run
# <KILN_OUT>/cmake/bin/tests/sfi-stamp-check.exe <stage dir> <scratch dir> instead. It is built here with the core of that
# stage: give the KILN_OUT tools/package/stage.sh built it in.
#   KILN_OUT=<the stage's build> WINEPREFIX=<private prefix> native/core/tests/test-sfi-stamp.sh <stage dir>
set -euo pipefail
: "${KILN_OUT:?set KILN_OUT to the build output of the stage}"
: "${WINEPREFIX:?use a private absolute Wine prefix}"
[[ $WINEPREFIX == /* ]] || { printf 'WINEPREFIX must be absolute\n' >&2; exit 2; }
stage=$(realpath -e -- "${1:?usage: test-sfi-stamp.sh <stage dir>}")
here=$(cd "$(dirname "$0")" && pwd)
"$here/../../build.sh" sfi-stamp-check >/dev/null
exe=$KILN_OUT/cmake/bin/tests/sfi-stamp-check.exe
scratch=$KILN_OUT/sfi-stamp-check
mkdir -p "$scratch"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
win() { printf 'Z:%s' "${1//\//\\}"; }
rc=0
timeout 120s wine "$exe" "$(win "$stage")" "$(win "$scratch")" || rc=$?
wineserver -w
exit "$rc"
