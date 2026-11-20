#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln-dllmod's host (src/session.cpp) with test packages, under plain Wine (no game, no window: no X display, a Wayland
# socket that does not exist, winedbg off). Build first:
#   KILN_OUT=<dir> native/build.sh dllmod-host-test
#   KILN_OUT=<dir> native/plugins/dllmod/tests/run_host.sh [scenario...]
# Each scenario runs in its own process (the packages it loads stay loaded). Exit code 0 = all passed.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../../build}
bin=$out/cmake/bin/dllmod-tests
work=$out/dllmod-tests
export WINEPREFIX=$work/pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d" WAYLAND_DISPLAY=kiln-no-display
unset DISPLAY
mkdir -p "$work"
[[ -f $WINEPREFIX/system.reg ]] || wine wineboot -i >/dev/null 2>&1
scenarios=("$@")
((${#scenarios[@]})) || scenarios=(start fail fault rejected missing-lib already-loaded no-world settings-invalid no-export none)
failed=0
for s in "${scenarios[@]}"; do
    if ! wine "$bin/dllmod_host_test.exe" "Z:$bin" "Z:$work/run-$s" "$s"; then
        echo "scenario $s FAILED"
        failed=1
    fi
done
exit "$failed"
