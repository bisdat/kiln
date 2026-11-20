#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds hooks_typed_test.exe (the typed link kind of kiln.hooks) and runs it under plain Wine (a scratch prefix under
# $KILN_OUT; no game, no window). Exit code 0 = everything passed. The run has a time limit (KILN_HOOKS_TEST_LIMIT
# seconds, 300 by default): a run that waits forever, as a lock-order deadlock in an install would, fails instead of
# hanging.
#   KILN_OUT=<build dir> native/core/tests/test-hooks-typed.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export KILN_OUT=${KILN_OUT:-$here/../../build}
"$here/../../build.sh" hooks_typed_test >/dev/null
export WINEPREFIX=$KILN_OUT/hooksv2-pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d"
# no window may reach the desktop: an unset WAYLAND_DISPLAY means the desktop's wayland-0
unset DISPLAY; export WAYLAND_DISPLAY=kiln-no-display
mkdir -p "$WINEPREFIX"
[[ -f $WINEPREFIX/system.reg ]] || wine wineboot -i >/dev/null 2>&1
exe=$KILN_OUT/cmake/bin/hooksv2/hooks_typed_test.exe
limit=${KILN_HOOKS_TEST_LIMIT:-300}
echo "== hooks_typed_test"
status=0
timeout -k 10 "$limit" wine "$exe" || status=$?
if ((status == 124 || status == 137)); then
  echo "FAIL hooks_typed_test timed out after ${limit}s: possible lock-order deadlock"
fi
exit "$status"
