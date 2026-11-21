#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# test infrastructure only. Headless Wine run of the stand-in steam_api64.dll's tests: the one-process
# unit test, then the two-process fabric test (A and B at once, each started with wine). Build first:
#   native/build.sh steam-standin steam-standin-test
#   KILN_OUT=<build dir> WINEPREFIX=<private absolute prefix> native/services/net/tests/steam_standin/run.sh
# Exit code 0 = every check passed.
set -euo pipefail
: "${KILN_OUT:?set KILN_OUT to the build output}"
: "${WINEPREFIX:?use a private absolute Wine prefix}"
[[ $WINEPREFIX == /* ]] || { printf 'WINEPREFIX must be absolute\n' >&2; exit 2; }
bin=$KILN_OUT/cmake/bin/tests/steam-standin
test_exe=$bin/steam-standin-test.exe
[[ -f $test_exe && -f $bin/steam_api64.dll ]] || { printf 'Build steam-standin and steam-standin-test first\n' >&2; exit 2; }
export WINEPREFIX WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d"
# no window may reach the desktop: an unset WAYLAND_DISPLAY means the desktop's wayland-0
unset DISPLAY; export WAYLAND_DISPLAY=kiln-no-display
mkdir -p "$WINEPREFIX"
[[ -f $WINEPREFIX/system.reg ]] || wine wineboot -i >/dev/null 2>&1
# a fabric port base of its own per run (port = base + SteamID % 1024; the unit test uses base + 2048 and up), below
# the ephemeral ranges of Linux (32768+) and Windows (49152+), so parallel runs and other programs rarely meet it
base=${STANDIN_PORT_BASE:-$(( 20000 + RANDOM % 9600 ))}
sync=$(mktemp -d "$KILN_OUT/steam-standin-sync.XXXXXX")
trap 'rm -rf "$sync"' EXIT
sync_windows="Z:${sync//\//\\}"
result=0
timeout 60s wine "$test_exe" unit "$((base + 1024 * 2))" || result=1
timeout 90s wine "$test_exe" fabric a "$base" "$sync_windows" & a=$!
timeout 90s wine "$test_exe" fabric b "$base" "$sync_windows" || result=1
wait "$a" || result=1
wineserver -w
exit "$result"
