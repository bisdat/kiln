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

# kiln.json's boot.testRefuse: {"<mod id>": "<collector>", ...} for the installed cases that use it, or removed
refuse() {
  local map=""
  for c in "$@"; do
    local r
    r=$(python3 -I "$here/fixtures.py" refuse "$c")
    [[ -n $r ]] && map+="boot-req-$c=$r "
  done
  python3 -I - "$L/kiln/kiln.json" $map <<'PY'
import json, sys
p = sys.argv[1]
j = json.load(open(p))
j.pop("boot", None)
pairs = dict(a.split("=", 1) for a in sys.argv[2:])
if pairs:
    j["boot"] = {"testRefuse": pairs}
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
  if [[ $role == required ]]; then
    local stop
    stop=$(grep -m1 "the start stops" "$log" | cut -c1-600)
    [[ $exitc == 1 ]] && echo "PASS $c exit status 1 (0x4B01)" || echo "FAIL $c exit status $exitc"
    [[ $stop == *"boot-req-$c"* ]] && echo "PASS $c stop line: ${stop#*kiln\] }" || echo "FAIL $c no stop line naming boot-req-$c: $stop"
    grep -q "boot-required-test: boot-req-$c loaded" "$log" && echo "FAIL $c its OnLoad ran" || echo "PASS $c its OnLoad never ran"
    grep -q "boot: generation 1 published" "$log" && [[ $c != knowledge-entry ]] && echo "FAIL $c generation 1 was published"
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
    start_and_collect "$LOGS/$name" 12
    refuse
    clean
    grep -E "boot-req-|boot-required-test: " "$LOGS/$name/kiln.log" | cut -c1-400 | head -n 80
    verdict "$LOGS/$name" optional
    ;;
  verdict)
    verdict "$LOGS/${2:?}" "${3:?}" "${4:-}"
    ;;
esac
