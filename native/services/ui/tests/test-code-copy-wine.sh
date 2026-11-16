#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# code_copy_win.h in a Windows process under plain Wine: no game, no display (tests/code_copy_wine/harness.cpp). A
# function of the test's own exe is copied from the exe file, as kiln.ui copies the game's footer function: the copy
# runs, reaches the thunk, uses the exe's data, and an exception unwinds through it.
#   KILN_OUT=<build dir> native/services/ui/tests/test-code-copy-wine.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?set KILN_OUT to an isolated build directory}"
export KILN_OUT
"$here/../../../build.sh" ui-code-copy-test >/dev/null
export WINEPREFIX=$KILN_OUT/ui-code-copy-pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display
mkdir -p "$WINEPREFIX"
[[ -f $WINEPREFIX/system.reg ]] || wine wineboot -i >/dev/null 2>&1
wine "$KILN_OUT/cmake/bin/ui-code-copy-test.exe" | tr -d '\r'
