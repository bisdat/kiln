#!/usr/bin/env bash
# content-stop-test's lab run (0.2.0-dev.29; docs/kiln/services/npcs.md "When the whole build fails"): twelve boots of one
# lab server on one save, with the fixture mods in mods/ and the lab switch kiln/control/knowledge.fail-build.
#   mods/tests/content-stop-test/lab.sh <lab name> <out dir> [labstart command]     (from the worktree's root)
# Before it: tools/lab/lab.sh new <name> <port>; KILN_OUT=… tools/deploy-server.sh <lab dir> --policy any.
#  - served (cs-good): the world loads, the build's summary line says what was served, no stop.
#  - read, compile, shared, assets, hook (cs-good, the switch with that word): the start stops before the world: exit
#    status 3 (0x4B03's low byte), the stop line and kiln.boot's line in kiln.log, kiln/state/content-refused.json naming
#    cs-good, boot.json "boot"."stop" with 0x4B03 and cs-good, progress.json "failed", the save byte for byte the same.
#  - broken (cs-good and cs-broken, no switch): the world loads, cs-broken is named with its error, cs-good is served,
#    and the previous start's content-refused.json moved to content-refused.last.json.
#  - late (cs-good, switch late): the world loads, then the stop is asked for; kiln.log says it could NOT be stopped and
#    the server keeps running (then stopped with SIGINT: status 0).
#  - empty-compile, empty-hook (cs-empty only, a quests file with nothing native; switch compile, hook): no stop.
#  - boot-compile, boot-hook (cs-good as a boot mod, "boot": true: a boot generation; switch compile, hook): the same
#    stop, from the published plan at the resource start and from the knowledge collector's prepare; boot.json says the
#    generation was deferred.
# Each boot's kiln.log goes to <out dir>/kiln-<phase>.log. Exit 1 when a boot did not do what it must.
set -uo pipefail
name=${1:?lab name} out=${2:?out dir} start=${3:-tools/lab/lab.sh start}
here=$(cd "$(dirname "$0")" && pwd)
srv=${KILN_LABS:-$HOME/srv/enshrouded/labs}/$name
[[ -d $srv/kiln ]] || { echo "lab.sh: $srv/kiln is missing (deploy Kiln first)" >&2; exit 2; }
ctl=$srv/kiln/control state=$srv/kiln/state
mkdir -p "$out" "$ctl"
save_hash() { (cd "$srv" && find savegame -type f -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum); }
use_mods() {  # the fixture mods of this boot, and only those
  rm -rf "$srv/kiln/mods/cs-good" "$srv/kiln/mods/cs-broken" "$srv/kiln/mods/cs-empty"
  for m in "$@"; do cp -r "$here/mods/$m" "$srv/kiln/mods/$m"; done
}
boot_mod() {  # cs-good takes part in a boot generation
  python3 -I -c 'import json,sys; p=sys.argv[1]; m=json.load(open(p)); m["boot"]=True; json.dump(m,open(p,"w"),indent=2)' "$srv/kiln/mods/cs-good/kiln.mod.json"
}
log_has() { grep -qF -- "$1" "$out/kiln-$phase.log"; }
pass=0 fail=0 bad=0
chk() { if eval "$2"; then pass=$((pass + 1)); echo "  PASS $1"; else fail=$((fail + 1)); echo "  FAIL $1"; fi; }
up_phase() {  # the world must load; then SIGINT
  $start "$name" > "$out/start-$phase.log" 2>&1
  local rc=$?
  [[ $phase == late ]] && for _ in $(seq 1 90); do tr -d '\r' < "$srv/kiln/logs/kiln.log" | grep -qF "could NOT be stopped" && break; sleep 1; done
  alive=$(tools/lab/lab.sh pid "$name")
  tools/lab/lab.sh stop "$name" >> "$out/start-$phase.log" 2>&1
  for _ in $(seq 1 60); do [[ -f $state/exit-code.txt ]] && break; sleep 1; done
  tr -d '\r' < "$srv/kiln/logs/kiln.log" > "$out/kiln-$phase.log"
  tr -d '\r' < "$srv/logs/enshrouded_server.log" > "$out/game-$phase.log" 2>/dev/null
  code=$(cat "$state/exit-code.txt" 2>/dev/null)
  chk "the world loaded (lab start $rc)" '[[ $rc == 0 ]] && grep -q "LoadEcsScene to Run" "$out/game-$phase.log"'
  chk "no stop line" '! log_has "the start is stopped (pid" && ! log_has "boot: the start stops"'
  chk "no content-refused.json" '[[ ! -f $state/content-refused.json ]]'
  chk "exit status 0 after SIGINT: '$code'" '[[ $code == 0 ]]'
}
stop_phase() {  # $1: the why part; the world must not load
  local why=$1 before after
  before=$(save_hash)
  if $start "$name" > "$out/start-$phase.log" 2>&1; then echo "$phase: the server reached its world" >> "$out/start-$phase.log"; fi
  for _ in $(seq 1 120); do [[ -f $state/exit-code.txt ]] && break; sleep 1; done
  tools/lab/lab.sh stop "$name" >> "$out/start-$phase.log" 2>&1  # nothing should be left; a no-op then
  tr -d '\r' < "$srv/kiln/logs/kiln.log" > "$out/kiln-$phase.log"
  tr -d '\r' < "$srv/logs/enshrouded_server.log" > "$out/game-$phase.log" 2>/dev/null
  after=$(save_hash)
  code=$(cat "$state/exit-code.txt" 2>/dev/null)
  for f in content-refused boot progress; do cp "$state/$f.json" "$out/$f-$phase.json" 2>/dev/null; done
  printf '%s\n' "$before" > "$out/save-$phase.sha256"
  chk "exit status 3 (0x4B03): '$code'" '[[ $code == 3 ]]'
  chk "no world loaded" '! grep -q "LoadEcsScene to Run" "$out/game-$phase.log"'
  chk "the stop line in kiln.log, with what failed" 'log_has "knowledge: native: the start is stopped (pid" && log_has "exit code 0x4B03): the mods'"'"' native quests, knowledge and NPCs could not be built at all (" && log_has "$why" && log_has "; mods: cs-good."'
  chk "kiln.boot's stop line" 'log_has "boot: the start stops (exit code 0x4B03): kiln.knowledge: "'
  chk "content-refused.json, boot.json boot.stop and progress.json failed" 'python3 -I "$here/check_stop.py" "$out/content-refused-$phase.json" "$out/boot-$phase.json" "$out/progress-$phase.json" cs-good "$why"'
  chk "the save is byte for byte the same ($(printf '%s\n' "$before" | grep -c .) files)" '[[ -n $before && $before == "$after" ]]'
}
for phase in served read compile shared assets hook broken late empty-compile empty-hook boot-compile boot-hook; do
  rm -f "$ctl/knowledge.fail-build"
  pass=0 fail=0
  echo "== $phase"
  case $phase in
    served) use_mods cs-good; up_phase
      served=$(grep -o "knowledge: native: 1 mod(s): .*served by kiln.assets" "$out/kiln-$phase.log" | sed 's/^knowledge: native: 1 mod(s): //')
      chk "the build served cs-good: $served" '[[ $served == *"1 quest(s)"*"1 template(s)"* ]]' ;;
    read|compile|assets|hook) use_mods cs-good; echo "$phase" > "$ctl/knowledge.fail-build"
      case $phase in assets) stop_phase "kiln.assets refused the built GameKnowledgeResource" ;; *) stop_phase "lab switch kiln/control/knowledge.fail-build: $phase" ;; esac
      chk "the switch's warning" 'log_has "LAB SWITCH kiln/control/knowledge.fail-build is present ($phase)"' ;;
    shared) use_mods cs-good; echo shared > "$ctl/knowledge.fail-build"
      stop_phase "the mods' native quests are not served: " ;;
    broken) use_mods cs-good cs-broken; up_phase
      chk "cs-broken named with its error" 'log_has "knowledge: native: cs-broken: knowledge cs_bad: type must be bool, counter or flags"'
      chk "cs-good served, as in the served boot (cs-broken added nothing)" 'log_has "knowledge: native: 2 mod(s): $served"'
      chk "the previous stop moved aside" 'log_has "native: the previous start was stopped because" && [[ -f $state/content-refused.last.json ]]' ;;
    late) use_mods cs-good; echo late > "$ctl/knowledge.fail-build"; up_phase
      chk "kiln.log: the start could NOT be stopped" 'log_has "knowledge: native: the start could NOT be stopped: a world already ran in this process" && log_has "mods: cs-good"'
      chk "the server was still running after it: pid '$alive'" '[[ -n $alive ]]' ;;
    empty-compile|empty-hook) use_mods cs-empty; echo "${phase#empty-}" > "$ctl/knowledge.fail-build"; up_phase
      chk "the switch was on" 'log_has "LAB SWITCH kiln/control/knowledge.fail-build is present (${phase#empty-})"' ;;
    boot-compile|boot-hook) use_mods cs-good; boot_mod; echo "${phase#boot-}" > "$ctl/knowledge.fail-build"
      stop_phase "lab switch kiln/control/knowledge.fail-build: ${phase#boot-}"
      chk "a boot generation (boot.json deferred, cs-good a participant)" 'python3 -I -c "import json,sys; b=json.load(open(sys.argv[1]))[\"boot\"]; sys.exit(0 if b.get(\"deferred\") and any(o.get(\"id\")==\"cs-good\" for o in b.get(\"owners\",[])) else 1)" "$out/boot-$phase.json"' ;;
  esac
  echo "$phase: lab.sh: done: $pass PASS, $fail FAIL"
  ((fail == 0)) || bad=1
done
rm -f "$ctl/knowledge.fail-build"
use_mods
exit $bad
