#!/usr/bin/env bash
# roster-cap-test's lab run (package 17c; docs/kiln/services/npcs.md "The roster's size"): six boots of one lab server on
# one save, each with the roster gen_roster.py writes for it: 64, 65, 129, 65-off (the lab switch), 128, 128-restart.
#   mods/tests/roster-cap-test/lab.sh <lab name> <out dir> [labstart command]     (from the worktree's root)
# Before it: tools/lab/lab.sh new <name> <port> --save <a save>; build this mod into $KILN_OUT/mods/roster-cap-test;
# KILN_OUT=… tools/deploy-server.sh <lab dir> --policy any. Each boot's kiln.log goes to <out dir>/kiln-<phase>.log.
# Stops the lab after each boot (SIGINT: the game saves). Exit 1 when a boot printed a FAIL line or no summary.
set -uo pipefail
name=${1:?lab name} out=${2:?out dir} start=${3:-tools/lab/lab.sh start}
here=$(cd "$(dirname "$0")" && pwd)
srv=${KILN_LABS:-$HOME/srv/enshrouded/labs}/$name
mod=$srv/kiln/mods/roster-cap-test
[[ -d $mod ]] || { echo "lab.sh: $mod is missing (deploy the mod first)" >&2; exit 2; }
mkdir -p "$out" "$srv/kiln/control" "$srv/kiln/data/kiln.knowledge"
touch "$srv/kiln/control/systems.parallel"  # kiln.world's lab parallel switch (read at start)
bad=0
for phase in 64 65 129 65-off 128 128-restart; do
  python3 -I "$here/gen_roster.py" "$phase" "$mod"
  if [[ $phase == 65-off ]]; then touch "$srv/kiln/data/kiln.knowledge/roster-64"; else rm -f "$srv/kiln/data/kiln.knowledge/roster-64"; fi
  $start "$name" > "$out/start-$phase.log" 2>&1 || { echo "lab.sh: $phase: the lab did not start (see $out/start-$phase.log)"; bad=1; break; }
  done_line="roster-cap-test: done: phase $phase:"
  for _ in $(seq 1 600); do
    tr -d '\r' < "$srv/kiln/logs/kiln.log" 2>/dev/null | grep -qF "$done_line" && break
    sleep 1
  done
  tools/lab/lab.sh stop "$name" >> "$out/start-$phase.log" 2>&1
  tr -d '\r' < "$srv/kiln/logs/kiln.log" > "$out/kiln-$phase.log"
  summary=$(grep -F "$done_line" "$out/kiln-$phase.log" | tail -1)
  echo "$phase: ${summary:-NO SUMMARY}"
  grep -F "roster-cap-test: FAIL" "$out/kiln-$phase.log" | head -20
  if [[ -z $summary || $summary != *", 0 FAIL"* ]]; then bad=1; fi
done
exit $bad
