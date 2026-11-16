#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Headless Wine run of the real kiln-engine.dll in a stand-in host. Build first: native/build.sh kiln-engine engine-host.
set -euo pipefail
: "${KILN_OUT:?set KILN_OUT to the build output}"
: "${WINEPREFIX:?use a private absolute Wine prefix}"
[[ $WINEPREFIX == /* ]] || { printf 'WINEPREFIX must be absolute\n' >&2; exit 2; }
host=$KILN_OUT/cmake/bin/engine-host.exe
dll=$KILN_OUT/cmake/bin/services/kiln-engine.dll
[[ -f $host && -f $dll ]] || { printf 'Build kiln-engine and engine-host first\n' >&2; exit 2; }
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
dll_windows="Z:${dll//\//\\}"
set +e
result=0
for mode in kfc4 none; do
  timeout 60s wine "$host" "$dll_windows" "$mode" || result=1
done
wineserver -w
exit "$result"
