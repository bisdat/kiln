#!/usr/bin/env bash
# roster-cap-test's lab run (package 17c; docs/kiln/services/npcs.md "The roster's size"): nine boots of one lab server on
# one save, each with the roster gen_roster.py writes for it: 64, 65, 129, 65-refused, 129-boot, 129-boot-required, 128,
# 128-restart, 128-control.
#   mods/tests/roster-cap-test/lab.sh <lab name> <out dir> [labstart command]     (from the worktree's root)
# Before it: tools/lab/lab.sh new <name> <port> --save <a save>; build this mod into $KILN_OUT/mods/roster-cap-test;
# KILN_OUT=… tools/deploy-server.sh <lab dir> --policy any. Each boot's kiln.log goes to <out dir>/kiln-<phase>.log.
#  - 129, 65-refused, 129-boot and 129-boot-required (the mod a boot mod, not required and required: a boot
#    generation) must stop at the start: no world ('LoadEcsScene to Run' absent), exit status 2 (0x4B02's low byte),
#    the refusal line and kiln.boot's stop line in kiln.log (never "the start goes on"), kiln/state/roster-refused.json
#    naming the mod, boot.json "boot"."stop" with 0x4B02 and the mod, progress.json "failed", the save byte for byte
#    the same; in the boot phases boot.json says the generation was deferred and how the mod took part.
#  - 128-control is the negative control (kiln/control/roster.no-pages with roster.lab): it passes only when exactly its
#    two page-2 assertions fail. In the 128 boot roster.no-pages lies there without roster.lab and must be ignored.
#  - every other boot: the mod's summary with 0 FAIL.
# Stops the lab after each boot (SIGINT: the game saves). Exit 1 when a boot did not do what it must.
set -uo pipefail
name=${1:?lab name} out=${2:?out dir} start=${3:-tools/lab/lab.sh start}
here=$(cd "$(dirname "$0")" && pwd)
srv=${KILN_LABS:-$HOME/srv/enshrouded/labs}/$name
mod=$srv/kiln/mods/roster-cap-test
[[ -d $mod ]] || { echo "lab.sh: $mod is missing (deploy the mod first)" >&2; exit 2; }
ctl=$srv/kiln/control
mkdir -p "$out" "$ctl"
touch "$ctl/systems.parallel"  # kiln.world's lab parallel switch (read at start)
save_hash() { (cd "$srv" && find savegame -type f -print0 | sort -z | xargs -0 sha256sum); }
bad=0
for phase in 64 65 129 65-refused 129-boot 129-boot-required 128 128-restart 128-control; do
  python3 -I "$here/gen_roster.py" "$phase" "$mod"
  rm -f "$ctl/roster.fail-install" "$ctl/roster.no-pages" "$ctl/roster.lab" "$ctl/roster.drop-page" "$srv/kiln/state/roster-refused.json"
  case $phase in
    65-refused) touch "$ctl/roster.fail-install" ;;
    128) touch "$ctl/roster.no-pages" ;;  # without roster.lab: ignored (the 128 boot must pass with its page)
    128-restart) touch "$ctl/roster.lab" ;;
    128-control) touch "$ctl/roster.no-pages" "$ctl/roster.lab" ;;  # the negative control runs only with roster.lab
  esac
  if [[ $phase == 129* || $phase == 65-refused ]]; then
    before=$(save_hash)
    if $start "$name" > "$out/start-$phase.log" 2>&1; then
      echo "$phase: FAIL the server reached its world"; bad=1
      tools/lab/lab.sh stop "$name" >> "$out/start-$phase.log" 2>&1
    fi
    for _ in $(seq 1 120); do [[ -f $srv/kiln/state/exit-code.txt ]] && break; sleep 1; done
    tools/lab/lab.sh stop "$name" >> "$out/start-$phase.log" 2>&1  # nothing should be left; a no-op then
    tr -d '\r' < "$srv/kiln/logs/kiln.log" > "$out/kiln-$phase.log"
    tr -d '\r' < "$srv/logs/enshrouded_server.log" > "$out/game-$phase.log" 2>/dev/null
    after=$(save_hash)
    rc=$(cat "$srv/kiln/state/exit-code.txt" 2>/dev/null)
    cp "$srv/kiln/state/roster-refused.json" "$out/roster-refused-$phase.json" 2>/dev/null
    cp "$srv/kiln/state/boot.json" "$out/boot-$phase.json" 2>/dev/null
    cp "$srv/kiln/state/progress.json" "$out/progress-$phase.json" 2>/dev/null
    printf '%s\n' "$before" > "$out/save-$phase.sha256"
    if [[ $phase == 129* ]]; then text="the roster holds at most 128 NPCs (two 64-bit unlock pages): 35 of the game's + 94 new"
    else text="the roster holds at most 64 NPCs (a 64-bit unlock mask): 35 of the game's + 30 new (the second unlock page is not available here: the hooks were not installed (lab switch kiln/control/roster.fail-install))"; fi
    pass=0 fail=0
    chk() { if eval "$2"; then pass=$((pass + 1)); echo "  PASS $1"; else fail=$((fail + 1)); echo "  FAIL $1"; fi; }
    {
      chk "exit status 2 (0x4B02): '$rc'" '[[ $rc == 2 ]]'
      chk "no world loaded" '! grep -q "LoadEcsScene to Run" "$out/game-$phase.log"'
      chk "the refusal line in kiln.log" 'grep -qF "roster: the start is stopped (pid" "$out/kiln-$phase.log" && grep -qF "$text" "$out/kiln-$phase.log"'
      chk "kiln/state/roster-refused.json names the mod" 'python3 -I -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d[\"refused\"] and d[\"mods\"]==[\"roster-cap-test\"] and d[\"exitCode\"]==0x4B02 and d[\"unixExitStatus\"]==2 and d[\"restart\"] is False and d[\"bootStop\"] else 1)" "$out/roster-refused-$phase.json"'
      chk "kiln.boot's stop line, and no 'the start goes on'" 'grep -qF "boot: the start stops (exit code 0x4B02): kiln.knowledge: " "$out/kiln-$phase.log" && ! grep -qF "the start goes on without it" "$out/kiln-$phase.log"'
      chk "boot.json boot.stop: 0x4B02, the mod; progress.json failed" 'python3 -I "$here/stop_files.py" "$out/boot-$phase.json" "$out/progress-$phase.json" "$phase"'
      chk "the save is byte for byte the same ($(printf '%s\n' "$before" | wc -l) files)" '[[ -n $before && $before == "$after" ]]'
      chk "the mod did not run" '! grep -qF "roster-cap-test: phase" "$out/kiln-$phase.log"'
    } > "$out/checks-$phase.txt"
    cat "$out/checks-$phase.txt"
    echo "$phase: lab.sh: done: $pass PASS, $fail FAIL"
    ((fail == 0)) || bad=1
    continue
  fi
  $start "$name" > "$out/start-$phase.log" 2>&1 || { echo "lab.sh: $phase: the lab did not start (see $out/start-$phase.log)"; bad=1; break; }
  done_line="roster-cap-test: done: phase $phase:"
  for _ in $(seq 1 900); do
    tr -d '\r' < "$srv/kiln/logs/kiln.log" 2>/dev/null | grep -qF "$done_line" && break
    sleep 1
  done
  tools/lab/lab.sh stop "$name" >> "$out/start-$phase.log" 2>&1
  tr -d '\r' < "$srv/kiln/logs/kiln.log" > "$out/kiln-$phase.log"
  summary=$(grep -F "$done_line" "$out/kiln-$phase.log" | tail -1)
  echo "$phase: ${summary:-NO SUMMARY}"
  grep -F "roster-cap-test: FAIL" "$out/kiln-$phase.log" | head -20
  if [[ $phase == 128-control ]]; then
    # the negative control: exactly the two page-2 assertions fail, nothing else
    if [[ $summary == *", 2 FAIL"* ]] && grep -qF "FAIL control: a server world token and page 2 read from the snapshot" "$out/kiln-$phase.log" &&
       grep -qF "FAIL control: the game destroyed the locked page-2 entry's NPC" "$out/kiln-$phase.log"; then
      echo "128-control: the negative control failed where it must (the two page-2 assertions)"
    else
      echo "128-control: FAIL the negative control did not fail exactly where it must"; bad=1
    fi
    continue
  fi
  if [[ -z $summary || $summary != *", 0 FAIL"* ]]; then bad=1; fi
  if [[ $phase == 128 ]]; then
    if grep -qF "kiln/control/roster.no-pages is present but IGNORED" "$out/kiln-$phase.log" &&
       ! grep -qF "LAB NEGATIVE CONTROL: a mod's roster needs" "$out/kiln-$phase.log"; then
      echo "128: roster.no-pages without roster.lab was ignored, with its warning"
    else
      echo "128: FAIL roster.no-pages without roster.lab was not ignored"; bad=1
    fi
  fi
done
rm -f "$ctl/roster.fail-install" "$ctl/roster.no-pages" "$ctl/roster.lab" "$ctl/roster.drop-page"
exit $bad
