#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Lab run of Kiln tasks and their world reads and writes on a dedicated server (tasks-lab-test + tasks-lab-driver):
#   mods/tests/tasks-lab-test/run.sh <lab> [--load-seconds N] [--cpu-load] [--no-deploy]
# The lab must exist (tools/lab/lab.sh new <lab> <port> --save <a world with a base>) and be stopped. Builds both mods
# into $KILN_OUT/mods and deploys Kiln with tools/deploy-server.sh --no-build (KILN_DEPLOY_BUILD=1 builds it too),
# starts the lab, waits for the driver's "READY exit", stops the lab with tasks in flight and checks the exit lines.
# --cpu-load runs tools/priority-kit/cpu-load.sh on every CPU for the whole run.
# Prints the PASS/FAIL lines; exit 1 on any FAIL or a missing line.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
lab=${1:?usage: run.sh <lab> [--load-seconds N] [--cpu-load] [--no-deploy]}; shift
load=90 cpu=0 deploy=1
while (($#)); do
  case $1 in
    --load-seconds) load=$2; shift 2 ;;
    --cpu-load) cpu=1; shift ;;
    --no-deploy) deploy=0; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
server=$HOME/srv/enshrouded/labs/$lab
[[ -f $server/enshrouded_server.exe ]] || { echo "no lab $lab" >&2; exit 1; }
export KILN_OUT=${KILN_OUT:-$root/.out/tasks-lab}
if ((deploy)); then
  for m in tasks-lab-test:TasksLabTest tasks-lab-driver:TasksLabDriver; do
    id=${m%%:*} proj=${m#*:}
    dotnet build "$root/mods/tests/$id/$proj.csproj" -c Release -o "$KILN_OUT/mods/$id" --artifacts-path "$KILN_OUT/dotnet" -v q -nologo >/dev/null
  done
  if [[ ${KILN_DEPLOY_BUILD:-0} == 1 ]]; then "$root/tools/deploy-server.sh" "$server" --policy any
  else "$root/tools/deploy-server.sh" "$server" --policy any --no-build; fi
fi
data=$server/kiln/data/tasks-lab-test
mkdir -p "$data"
rm -f "$data"/ready-* "$data"/unwound-* "$data"/ignored-* "$data"/loads.txt "$data"/character.txt
echo "$load" > "$data/load-seconds"
log=$server/kiln/logs/kiln.log
start_line=0  # kiln.log starts afresh at every boot (the last one is kiln.prev.log)
cpu_pid=
if ((cpu)); then
  "$root/tools/priority-kit/cpu-load.sh" --load 3000 --idle 1 --cycles 1 --max-minutes 60 --out "$KILN_OUT/cpu-load.csv" >/dev/null 2>&1 &
  cpu_pid=$!
fi
stop_cpu() { if [[ -n $cpu_pid ]]; then kill "$cpu_pid" 2>/dev/null || true; wait "$cpu_pid" 2>/dev/null || true; cpu_pid=; fi; }
trap stop_cpu EXIT
(cd "$root" && /mnt/Dongus/kiln-wave/tools/labstart "$lab" >/dev/null 2>&1 || "$root/tools/lab/lab.sh" start "$lab")
deadline=$((SECONDS + 1800)) stalled=0
new() { if [[ -f $log ]]; then tail -n +"$((start_line + 1))" "$log"; fi; }
until new | grep -q "tasks-lab-driver: READY exit\|tasks-lab-driver done:"; do
  ((SECONDS < deadline)) || { echo "FAIL the run did not reach the driver's READY exit in 30 minutes"; stalled=1; break; }
  "$root/tools/lab/lab.sh" pid "$lab" | grep -q . || { echo "FAIL the lab server stopped during the run"; stalled=1; break; }
  sleep 5
done
sleep 3
stop_cpu
stop_at=$(date +%s)
stop_out=$("$root/tools/lab/lab.sh" stop "$lab" 2>&1 || true); echo "$stop_out"
for _ in $(seq 60); do "$root/tools/lab/lab.sh" pid "$lab" | grep -q . || break; sleep 1; done
out=$(new)
grep -E "tasks-lab-(test|driver)(: (PASS|FAIL|INFO)| done)" <<<"$out" || true
fail=0
grep -q "tasks-lab-test: FAIL\|tasks-lab-driver: FAIL" <<<"$out" && fail=1
((stalled)) && fail=1
check() { if grep -qE "$2" <<<"$out"; then echo "PASS exit: $1"; else echo "FAIL exit: $1"; fail=1; fi; }
check "the unload reported the task that ignored its cancel" "tasks: mod tasks-lab-test: .*did not stop within"
check "the first load context unloaded after its tasks stopped" "tasks: mod tasks-lab-test's load context unloaded after its tasks stopped"
check "the pool parked at exit" "kiln.tasks: parked at exit"
check "the unload sequence finished" "unload: done in"
if grep -qiE "Unhandled|access violation|crash report" <<<"$out"; then echo "FAIL exit: no crash line"; fail=1; else echo "PASS exit: no crash line"; fi
if "$root/tools/lab/lab.sh" pid "$lab" | grep -q .; then echo "FAIL exit: the server is still running"; fail=1; fi
grep -q "kill -9\|still running after" <<<"$stop_out" && { echo "FAIL exit: the server did not stop on SIGINT (lab.sh killed it)"; fail=1; }
echo "INFO exit: the stop took $(( $(date +%s) - stop_at )) s"
exit $fail
