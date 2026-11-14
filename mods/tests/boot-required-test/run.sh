#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# A required boot mod gets all of its boot content or the start stops (08 fix round, item 1). One dedicated-server lab; each
# start installs fixture mods made by fixtures.py (one bad entry for one collector each, next to good ones):
#   required  the start must stop before the game's entry point (the knowledge build's case: when the game's resources come
#             up) with exit status 1 (0x4B01's low byte under Wine), the stop line must name the mod, the entry and the
#             reason, and the fixture's OnLoad must never run;
#   optional  the start goes on: the fixture loads, kiln.log names the skipped entry (content, vfx and patches drop the mod
#             whole, as they always did).
#
# Run from the root of the worktree. Environment:
#   LAB       lab name (tools/lab/lab.sh new <name> <port> made it)        KILN_OUT  the build (deploy-server.sh's)
#   LOGS      where each start's logs and state go                         SLOT      a prefix for heavy commands (may be empty)
#   LABSTART  the command that starts the lab (default: tools/lab/lab.sh start)
#
#   run.sh build                     build the fixture assembly and deploy Kiln into the lab (the lab is stopped)
#   run.sh case <case> required|optional [<name>]   one start with that fixture only; its folder is $LOGS/<name>
#   run.sh optional [<name>]         one start with every case's fixture, all with "boot": true
#   run.sh verdict <name> required|optional [<case>]  PASS or FAIL lines for one start's folder
#   run.sh client <folder>           the fixtures only a game client can show, required, into <folder> (guided check
#                                    08-boot.required-client; needs KILN_OUT for the fixture assembly only)
# A required fixture's kind (fixtures.py kind): "stops" before the game's entry point, "late" after it (when the game's data or a
# world loads; generation 1 is published first), "starts" (nothing of it is bad: the start goes on).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${LAB:?}" "${KILN_OUT:?}" "${LOGS:?}"
SLOT=${SLOT:-}
LABSTART=${LABSTART:-tools/lab/lab.sh start}
L=$HOME/srv/enshrouded/labs/$LAB
F=$KILN_OUT/boot-required/fixture
mkdir -p "$LOGS"

cases() { python3 -I "$here/fixtures.py" list | cut -f1; }

clean() {
  for d in "$L"/kiln/mods/boot-req-*; do
    [[ -d $d ]] || continue
    chmod -R u+rwX "$d" && rm -rf "$d"
  done
}

install() {  # install <case> <role>
  local d=$L/kiln/mods/boot-req-$1
  python3 -I "$here/fixtures.py" make "$d" "$1" "$2" && cp "$F/BootRequiredFixture.dll" "$d/"
}

# kiln.json's boot.testRefuse and boot.testFault: {"<mod id>": "<collector>", ...} for the installed cases that use them, or removed
refuse() {
  local map=""
  for c in "$@"; do
    local r f
    r=$(python3 -I "$here/fixtures.py" refuse "$c")
    f=$(python3 -I "$here/fixtures.py" fault "$c")
    [[ -n $r ]] && map+="testRefuse:boot-req-$c=$r "
    [[ -n $f ]] && map+="testFault:boot-req-$c=$f "
  done
  python3 -I - "$L/kiln/kiln.json" $map <<'PY'
import json, sys
p = sys.argv[1]
j = json.load(open(p))
j.pop("boot", None)
boot = {}
for a in sys.argv[2:]:
    switch, pair = a.split(":", 1)
    mod, collector = pair.split("=", 1)
    boot.setdefault(switch, {})[mod] = collector
if boot:
    j["boot"] = boot
json.dump(j, open(p, "w"), indent=2)
PY
}

start_and_collect() {  # start_and_collect <folder> <expected loaded lines>
  local D=$1 want=$2 log=$L/kiln/logs/kiln.log
  rm -rf "$D" && mkdir -p "$D"
  rm -f "$log" "$L/kiln/state/boot.json" "$L/kiln/state/exit-code.txt"
  $LABSTART "$LAB" 2>&1 | tail -n 1
  for i in $(seq 1 120); do
    grep -q "the start stops" "$log" 2>/dev/null && break
    [[ $(grep -c "boot-required-test: boot-req-.* loaded" "$log" 2>/dev/null) -ge $want ]] && break
    [[ -z $(tools/lab/lab.sh pid "$LAB" 2>/dev/null) ]] && { echo "the lab process ended"; break; }
    sleep 5
  done
  sleep 3
  tools/lab/lab.sh stop "$LAB" | tail -n 1
  for i in $(seq 1 20); do [[ -f $L/kiln/state/exit-code.txt ]] && break; sleep 1; done
  cp "$log" "$L/kiln/state/boot.json" "$L/kiln/state/exit-code.txt" "$D/" 2>/dev/null
  echo "exit status: $(cat "$D/exit-code.txt" 2>/dev/null || echo none)"
}

verdict() {  # verdict <folder> <role> [<case>]
  local D=$1 role=$2 c=${3:-} log=$1/kiln.log
  local exitc
  exitc=$(cat "$D/exit-code.txt" 2>/dev/null || echo none)
  local kind
  kind=$(python3 -I "$here/fixtures.py" kind "$c")
  if [[ $role == required && $kind == starts ]]; then
    [[ $exitc == 0 || $exitc == none ]] && echo "PASS $c the start went on (exit status $exitc)" || echo "FAIL $c exit status $exitc"
    grep -q "the start stops" "$log" && echo "FAIL $c the start stopped: $(grep -m1 'the start stops' "$log" | cut -c1-300)"
    grep -q "boot-required-test: boot-req-$c loaded: boot state Committed" "$log" && echo "PASS $c loaded (Committed)" || echo "FAIL $c did not load as Committed"
    [[ $c == patches-comma ]] && { grep -q "resource patch boot-req-patches-comma/xp: .*experienceNeedPerLevel\[2\]\" = 2600" "$log" &&
      echo "PASS $c its patch from a file with comments and trailing commas was applied" || echo "FAIL $c its patch was not applied"; }
  elif [[ $role == required ]]; then
    local stop
    stop=$(grep -m1 "the start stops" "$log" | cut -c1-600)
    [[ $exitc == 1 ]] && echo "PASS $c exit status 1 (0x4B01)" || echo "FAIL $c exit status $exitc"
    [[ $stop == *"boot-req-$c"* ]] && echo "PASS $c stop line: ${stop#*kiln\] }" || echo "FAIL $c no stop line naming boot-req-$c: $stop"
    if [[ $kind == late ]]; then  # found after the entry point: OnLoad may have run before (a world's load comes after it)
      [[ -n $stop && $stop != *"before the game's entry point"* ]] && echo "PASS $c stopped after the entry point, at the load that found it" ||
        echo "FAIL $c did not stop after the entry point"
      grep -q "boot-required-test: boot-req-$c loaded" "$log" && echo "INFO $c its OnLoad ran before the stop" || echo "PASS $c its OnLoad never ran"
      # a stop at world load: the server world may have begun to tick (its registries load then), no player is in it yet
      grep -q "tick: server world .* started" "$log" && echo "INFO $c the server world had begun to load (its first ticks) when it stopped"
      grep -qiE "player .* (joined|connected)" "$log" && echo "FAIL $c a player was in the world before the stop" ||
        echo "PASS $c no player was in the world"
    else
      grep -q "boot-required-test: boot-req-$c loaded" "$log" && echo "FAIL $c its OnLoad ran" || echo "PASS $c its OnLoad never ran"
      grep -q "boot: generation 1 published" "$log" && echo "FAIL $c generation 1 was published"
    fi
  else
    [[ $exitc == 0 || $exitc == none ]] && echo "PASS optional start ended normally (exit status $exitc)" || echo "FAIL optional exit status $exitc"
    grep -q "the start stops" "$log" && echo "FAIL optional the start stopped: $(grep -m1 'the start stops' "$log" | cut -c1-300)"
    for c in $(cases); do
      case $c in
        content-entry | vfx-entry | patches-entry)
          grep -q "boot-req-$c is dropped from generation 1" "$log" && echo "PASS optional $c dropped whole (as before)" ||
            echo "FAIL optional $c not dropped" ;;
        *)
          grep -q "boot-required-test: boot-req-$c loaded: boot state Committed" "$log" && echo "PASS optional $c loaded (Committed)" ||
            echo "FAIL optional $c did not load as Committed" ;;
      esac
    done
    grep -q "resource patch boot-req-patches-comma/xp: .*experienceNeedPerLevel\[2\]\" = 2600" "$log" &&
      echo "PASS optional patches-comma its patch from a file with comments and trailing commas was applied" ||
      echo "FAIL optional patches-comma its patch was not applied"
    grep -q "new content: item .* not registered in the server world: lab switch boot.testFault" "$log" &&
      echo "PASS optional content-bind runs without its item (the world refused it at registration)" ||
      echo "FAIL optional content-bind: no refused registration in kiln.log"
  fi
}

case ${1:?build|case|optional|verdict} in
  build)
    $SLOT dotnet build "$here/BootRequiredFixture.csproj" -c Release -m:4 -o "$F" --artifacts-path "$KILN_OUT/dotnet" -v q -nologo 2>&1 |
      grep -E ' error |rror\(s\)' | sort -u
    [[ -f $F/BootRequiredFixture.dll ]] && echo "built $F" || { echo "the fixture did not build"; exit 1; }
    clean
    $SLOT tools/deploy-server.sh "$L" --policy any > "$LOGS/deploy.log" 2>&1; echo "deploy exit $?"
    ;;
  case)
    c=${2:?case} role=${3:?required or optional} name=${4:-$2-$3}
    clean
    install "$c" "$role" || exit 1
    refuse "$c"
    start_and_collect "$LOGS/$name" 1
    refuse
    clean
    grep -E "\] boot: |boot-required-test: |boot-req-$c" "$LOGS/$name/kiln.log" | cut -c1-400 | tail -n 30
    verdict "$LOGS/$name" "$role" "$c"
    ;;
  optional)
    name=${2:-optional}
    clean
    all=$(cases)
    for c in $all; do install "$c" optional || exit 1; done
    refuse $all
    start_and_collect "$LOGS/$name" $(($(echo $all | wc -w) - 3))
    refuse
    clean
    grep -E "boot-req-|boot-required-test: " "$LOGS/$name/kiln.log" | cut -c1-400 | head -n 80
    verdict "$LOGS/$name" optional
    ;;
  verdict)
    verdict "$LOGS/${2:?}" "${3:?}" "${4:-}"
    ;;
  client)
    dest=${2:?folder}
    for c in $(python3 -I "$here/fixtures.py" client); do
      python3 -I "$here/fixtures.py" make "$dest/boot-req-$c" "$c" required && cp "$F/BootRequiredFixture.dll" "$dest/boot-req-$c/" && echo "made $dest/boot-req-$c"
    done
    ;;
esac
