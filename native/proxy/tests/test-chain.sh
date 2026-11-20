#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The proxy's chain loader under Wine, without a game. KILN_TEST_WINE names the wine to run it with (default: wine
# from PATH), for example GE-Proton's <tool>/files/bin/wine; another wine gets a prefix of its own under $KILN_OUT.
# Three runs: Wine's default load order, xinput9_1_0=b (Wine's builtin in place of a chained xinput9_1_0.dll: the
# proxy must warn) and xinput9_1_0=n,b (the override that warning gives: the file loads as asked).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export KILN_OUT=${KILN_OUT:-$here/../../build}
"$here/../../build.sh" proxy-chain-test
wine=${KILN_TEST_WINE:-wine}
version=$("$wine" --version)
pfx=$KILN_OUT/proxy-chain-pfx
[[ -z ${KILN_TEST_WINE:-} ]] || pfx+=-$(tr -cs 'A-Za-z0-9.' '-' <<< "$version")
export WINEPREFIX=$pfx WINEDEBUG=-all WINEDLLOVERRIDES='winedbg.exe=d'
unset DISPLAY; export WAYLAND_DISPLAY=kiln-no-display
mkdir -p "$WINEPREFIX"
[[ -f $WINEPREFIX/system.reg ]] || "$wine" wineboot -i >/dev/null 2>&1
scratches=()
trap 'rm -rf "${scratches[@]}"' EXIT
run() {  # run <WINEDLLOVERRIDES> [<KILN_TEST_EXPECT_SWAP: 1 or 0>]
  local scratch
  scratch=$(mktemp -d "$KILN_OUT/proxy-chain.XXXXXX")
  scratches+=("$scratch")
  echo "== $version, WINEDLLOVERRIDES=$1${2:+, KILN_TEST_EXPECT_SWAP=$2}"
  env WINEDLLOVERRIDES="$1" ${2:+KILN_TEST_EXPECT_SWAP=$2} "$wine" "$KILN_OUT/cmake/bin/proxy-chain-test.exe" "$scratch" \
    "$KILN_OUT/cmake/bin/proxy-test-addon.dll" "$KILN_OUT/cmake/bin/proxy-test-plain.dll"
}
run 'winedbg.exe=d'
run 'winedbg.exe=d;xinput9_1_0=b' 1
run 'winedbg.exe=d;xinput9_1_0=n,b' 0
