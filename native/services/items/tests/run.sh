#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.items host tests under plain Wine, no game: the item-instance stamps and reads (instances_test.cpp), the gear
# options (pide_test.cpp), the equip logic (equip_test.cpp), the forge actions' logic (gems_test.cpp), the station actions' logic (stations_test.cpp), the ingredient rows' count hook (stock_ui_native_test.cpp), the gear writes' logic (gear_test.cpp), the container renames' text check and steps against a fake string table (rename_test.cpp), the equipment-set snapshot, store, planner, executor and request order (setsnap_test.cpp, setplan_test.cpp, setexec_test.cpp, setorder_test.cpp) and the moves, ledger, transfer
# rules, handlers, reports and hook chaining against a fake inventory world (move_test.cpp). What they prove is the
# service's own logic; the game's behaviour is the lab test mods/tests/items-test.
#   KILN_OUT=<build dir> native/services/items/tests/run.sh
# Exit code 0 = every check passed. The hook chain runs in two more processes (hooks are never removed).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export KILN_OUT=${KILN_OUT:-$here/../../../build}
"$here/../../../build.sh" items-move-test items-pide-test items-results-test items-instances-test items-equip-test items-setsnap-test items-setplan-test items-setexec-test items-setorder-test items-wear-test items-toolwear-test items-toolwear-table-test items-gems-test items-stations-test items-gear-test items-rename-test items-craft-sources-test items-craft-payment-test items-stock-ui-test items-dye-sources-test items-dyes-native-test items-storage-native-test >/dev/null
bin=$KILN_OUT/cmake/bin/items-tests
export WINEPREFIX=$KILN_OUT/items-tests-pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d"
# no window may reach the desktop: an unset WAYLAND_DISPLAY means the desktop's wayland-0
unset DISPLAY; export WAYLAND_DISPLAY=kiln-no-display
mkdir -p "$WINEPREFIX"
[[ -f $WINEPREFIX/system.reg ]] || wine wineboot -i >/dev/null 2>&1
fail=0
for run in "items-storage-native-test.exe" "items-move-test.exe storage" "items-rename-test.exe" "items-gear-test.exe" "items-stations-test.exe" "items-craft-sources-test.exe" "items-craft-payment-test.exe" "items-stock-ui-test.exe" "items-dye-sources-test.exe" "items-dyes-native-test.exe" "items-gems-test.exe" "items-wear-test.exe" "items-toolwear-test.exe" "items-toolwear-table-test.exe" "items-results-test.exe" "items-equip-test.exe" "items-setsnap-test.exe" "items-setplan-test.exe" "items-setexec-test.exe" "items-setorder-test.exe" "items-instances-test.exe" "items-pide-test.exe" "items-move-test.exe" "items-move-test.exe chain-before" "items-move-test.exe chain-after" "items-move-test.exe partial-install" "items-move-test.exe links" "items-move-test.exe review" "items-move-test.exe move-off" "items-move-test.exe stamps" "items-move-test.exe replace" "items-move-test.exe give-equip"; do
  echo "== $run"
  # shellcheck disable=SC2086
  wine "$bin/"$run || fail=1
done
# the approved builds of kiln.toolwear against the program catalog (no Wine, no game)
echo "== wear_catalog_test.py"
python3 "$here/wear_catalog_test.py" || fail=1
# the player storage sigdb rows on read-only exe copies (KILN_SERVER_EXE / KILN_CLIENT_EXE, read as files, never run);
# a side without a copy of the qualified build is skipped with a message
echo "== storage_sigdb_test.py"
python3 -I "$here/storage_sigdb_test.py" || fail=1
exit $fail
