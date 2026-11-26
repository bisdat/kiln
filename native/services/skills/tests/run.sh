#!/usr/bin/env bash
# Builds and runs kiln.skills' host tests: player_scan.h, which entities the shadow table maps every server step (the
# players' records every step, the entity-map walk only when the entity count moved or now and then), against a fake
# world: the same set as a walk every step would give, and how many walks it takes. No game; client_view.cpp's own
# test runs under Wine.
# The tree builder has its own host test: tools/kfc/tests/test_skilltree_slots.py.
#   native/services/skills/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/skills-host-test
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/.." -I"$here/../../../core/abi" "$here/test_player_scan.cpp" -o "$out/test_player_scan"
"$out/test_player_scan"
# kiln.skills.client's pure part (client_view_logic.h, 0.2.0-dev.31.13), and its sigdb rows on read-only exe copies
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$here/.." -I"$here/../../../core/abi" "$here/test_client_view.cpp" -o "$out/test_client_view"
"$out/test_client_view"
# client_view.cpp itself against a fake client world (review 31.13): cross-built (native/build.sh), run under plain Wine
"$here/../../../build.sh" skills-client-view-native-test >/dev/null
export WINEPREFIX=$KILN_OUT/skills-tests-pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d"
# no window may reach the desktop: an unset WAYLAND_DISPLAY means the desktop's wayland-0
unset DISPLAY; export WAYLAND_DISPLAY=kiln-no-display
mkdir -p "$WINEPREFIX"
[[ -f $WINEPREFIX/system.reg ]] || wine wineboot -i >/dev/null 2>&1
wine "$KILN_OUT/cmake/bin/skills-tests/skills-client-view-native-test.exe"
if [[ -n ${KILN_CLIENT_EXE:-} && -n ${KILN_SERVER_EXE:-} ]]; then
  python3 -I "$here/client_view_sigdb_test.py" --client "$KILN_CLIENT_EXE" --server "$KILN_SERVER_EXE"
else
  echo "SKIP client skill view signatures: supply KILN_CLIENT_EXE and KILN_SERVER_EXE static copies"
fi
