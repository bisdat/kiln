#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Headless Wine run of the real kiln-engine.dll in a stand-in host, then of kiln-engine-test.dll (same code, test facts)
# running the Scene gate over a fake engine, then the core's real lookup and stamp blocks against this build's DLLs
# (engine-core-lookup). Build first: native/build.sh (all) kiln-engine-test engine-host engine-core-lookup.
set -euo pipefail
: "${KILN_OUT:?set KILN_OUT to the build output}"
: "${WINEPREFIX:?use a private absolute Wine prefix}"
[[ $WINEPREFIX == /* ]] || { printf 'WINEPREFIX must be absolute\n' >&2; exit 2; }
host=$KILN_OUT/cmake/bin/engine-host.exe
dll=$KILN_OUT/cmake/bin/services/kiln-engine.dll
test_dll=$KILN_OUT/cmake/bin/tests/kiln-engine-test.dll
lookup=$KILN_OUT/cmake/bin/engine-core-lookup.exe
[[ -f $host && -f $dll && -f $test_dll && -f $lookup ]] ||
  { printf 'Build kiln-engine, kiln-engine-test, engine-host and engine-core-lookup first\n' >&2; exit 2; }
# another build's kiln-engine.dll and one from a build without build metadata (version only), both the same length,
# derived from this build's own KILN_BUILD (generated/kiln_build.h: any commit date, override or nonce)
stale=$KILN_OUT/engine-stale/kiln-engine-stale.dll
unstamped=$KILN_OUT/engine-stale/kiln-engine-unstamped.dll
mkdir -p "${stale%/*}"
here=$(cd "$(dirname "$0")" && pwd)
python3 -I "$here/stamp_fixtures.py" --self-test >/dev/null || { printf 'stamp_fixtures.py self-test failed\n' >&2; exit 2; }
python3 -I "$here/stamp_fixtures.py" "$KILN_OUT/cmake/generated/kiln_build.h" "$dll" "$stale" "$unstamped" || exit 2
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
dll_windows="Z:${dll//\//\\}"
test_windows="Z:${test_dll//\//\\}"
set +e
result=0
for mode in kfc4 none; do
  timeout 60s wine "$host" "$dll_windows" "$mode" || result=1
done
for mode in gate-server gate-client; do
  timeout 60s wine "$host" "$test_windows" "$mode" || result=1
done
bin_windows="Z:${KILN_OUT//\//\\}\\cmake\\bin"
timeout 120s wine "$lookup" "$bin_windows" "Z:${stale//\//\\}" "Z:${unstamped//\//\\}" "$dll_windows" || result=1
wineserver -w
exit "$result"
