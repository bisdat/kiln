#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Developer debugging's descriptor and control worker (src/debug_descriptor.cpp):
#   native/core/tests/test-debug-descriptor.sh               host test (ASan/UBSan), no Wine
#   native/core/tests/test-debug-descriptor.sh --lab <lab>   lab check on a dedicated server (below)
# Build output: $KILN_OUT/core-tests (KILN_OUT default native/build).
#
# --lab <lab>: the lab must exist (tools/lab/lab.sh new <lab> <port>) and have this tree's Kiln deployed
# (tools/deploy-server.sh). This script edits only the lab's kiln/kiln.json (put back at the end), starts the lab with
# $KILN_LAB_START (default: tools/lab/lab.sh start), stops it with tools/lab/lab.sh stop, and leaves it stopped:
#   1. "developer_debugging": true: the descriptor kiln/state/debug/<pid>.json appears; it validates against
#      schema/state-debug.schema.json (ajv, when found), its windowsPid and imageSha256 are boot.json's pid and exe
#      sha256, creationTime100ns is a decimal string, the pipe name is made of the pid and the nonce, and kiln.log has
#      the core's "debug: developer debugging is on" line naming that pipe. After the stop: whether the descriptor was
#      removed (a clean exit removes it; a stale one is what the resolver's creation-time check is for).
#   2. "developer_debugging": false: no descriptor, no "debug:" line, no control pipe named in kiln.log.
# Results: $KILN_OUT/debug-lab/lab.log (this script's own log: PASS/FAIL lines), with each run's kiln.log and files.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"

if [[ ${1:-} != --lab ]]; then
  cxx=${CXX:-$(command -v clang++ || command -v g++)}
  "$cxx" -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$here/../abi" -I"$here/../../third_party" -o "$out/debug_descriptor_test" "$here/debug_descriptor_test.cpp"
  # a worker that hangs fails the run instead of hanging it
  timeout 120 "$out/debug_descriptor_test"
  exit
fi

lab=${2:?usage: $0 --lab <lab>}
[[ $lab =~ ^[a-z0-9-]+$ ]] || { echo "a lab name is [a-z0-9-]+" >&2; exit 2; }
labs=${KILN_LABS:-${KILN_LAB_ROOT:-$HOME/srv/enshrouded}/labs}
server=$labs/$lab
[[ -f $server/enshrouded_server.exe && -d $server/kiln ]] || { echo "no lab $lab with Kiln deployed" >&2; exit 1; }
read -r -a start <<< "${KILN_LAB_START:-tools/lab/lab.sh start}"
work=${KILN_OUT:-$here/../../build}/debug-lab
rm -rf "$work"
mkdir -p "$work"
log=$work/lab.log
cd "$repo"
say() { echo "$*" | tee -a "$log"; }
fails=0
pass() { say "PASS $*"; }
fail() { say "FAIL $*"; fails=$((fails + 1)); }

cfg=$server/kiln/kiln.json
backup=$work/kiln.json.before
had_cfg=0
if [[ -f $cfg ]]; then cp "$cfg" "$backup"; had_cfg=1; fi
restore() {
  if ((had_cfg)); then cp "$backup" "$cfg"; else rm -f "$cfg"; fi
}
trap restore EXIT

# kiln.json with developer_debugging set, the lab's other keys kept
set_switch() {
  python3 -I - "$cfg" "$1" "$had_cfg" "$backup" <<'PY'
import json, sys
cfg, on, had, backup = sys.argv[1], sys.argv[2] == "on", sys.argv[3] == "1", sys.argv[4]
doc = {}
if had:
    text = open(backup, encoding="utf-8-sig").read()
    try:
        doc = json.loads(text)
    except ValueError:
        sys.exit("the lab's kiln.json has comments; edit it by hand")
doc["developer_debugging"] = on
for side in ("server", "client"):
    if isinstance(doc.get(side), dict):
        doc[side].pop("developer_debugging", None)
open(cfg, "w", encoding="utf-8").write(json.dumps(doc, indent=2) + "\n")
PY
}

klog=$server/kiln/logs/kiln.log
ddir=$server/kiln/state/debug

wait_for() {  # wait_for <seconds> <command...>
  local n=$1; shift
  for ((i = 0; i < n * 2; i++)); do "$@" && return 0; sleep 0.5; done
  return 1
}
booted() { [[ -f $server/kiln/state/boot.json ]] && grep -q "boot complete" "$klog" 2>/dev/null; }

run_lab() {  # run_lab <on|off>
  set_switch "$1"
  rm -f "$server/kiln/state/boot.json"
  rm -rf "$ddir"
  "${start[@]}" "$lab" >> "$work/start-$1.out" 2>&1
  if ! wait_for 240 booted; then fail "debug.lab-$1: the lab did not finish booting in 240 s"; return 1; fi
}

stop_lab() {
  tools/lab/lab.sh stop "$lab" >> "$work/stop.out" 2>&1 || true
  wait_for 60 bash -c "[[ -z \$(tools/lab/lab.sh pid '$lab' 2>/dev/null) ]]" || say "note: the lab still reports a pid after 60 s"
}

ajv_js=${AJV_2020:-}
if [[ -z $ajv_js ]]; then
  for c in "$repo/launcher/node_modules/ajv/dist/2020.js" "$HOME/Work/kiln/launcher/node_modules/ajv/dist/2020.js"; do
    [[ -f $c ]] && { ajv_js=$c; break; }
  done
fi

# ---- 1. on ----
say "lab $lab: developer_debugging true"
if run_lab on; then
  if wait_for 30 bash -c "compgen -G '$ddir/*.json' >/dev/null"; then
    desc=$(compgen -G "$ddir/*.json" | head -1)
    cp "$desc" "$work/descriptor.json"
    cp "$server/kiln/state/boot.json" "$work/boot-on.json"
    if python3 -I - "$desc" "$server/kiln/state/boot.json" "$klog" <<'PY' >> "$log" 2>&1
import json, os, re, sys
desc, boot, klog = sys.argv[1:4]
d = json.load(open(desc, encoding="utf-8"))
b = json.load(open(boot, encoding="utf-8"))
log = open(klog, encoding="utf-8", errors="replace").read()
bad = []
def need(ok, what):
    if not ok: bad.append(what)
need(d.get("format") == 1, "format 1")
need(os.path.basename(desc) == f"{d.get('windowsPid')}.json", "file named after windowsPid")
need(d.get("windowsPid") == b.get("pid"), f"windowsPid {d.get('windowsPid')} == boot.json pid {b.get('pid')}")
need(d.get("imageSha256") == b["exe"]["sha256"], "imageSha256 == boot.json exe.sha256")
need(isinstance(d.get("creationTime100ns"), str) and re.fullmatch(r"[1-9][0-9]*", d["creationTime100ns"]) is not None, "creationTime100ns decimal string")
need(re.fullmatch(r"[0-9a-f]{32}", d.get("sessionNonce", "")) is not None, "sessionNonce 32 hex")
need(d.get("pipe") == f"\\\\.\\pipe\\kiln-debug-{d.get('windowsPid')}-{d.get('sessionNonce')}", "pipe name")
need(d.get("side") == "server", "side server")
need(d.get("image", "").lower().endswith("enshrouded_server.exe"), "image is the server exe")
need(d.get("managedReady") is (b["clr"]["started"] is True), "managedReady == boot.json clr.started")
need(f"control pipe {d.get('pipe')}" in log, "kiln.log names the control pipe")
print("descriptor:", json.dumps(d))
if bad:
    print("mismatch:", "; ".join(bad))
    sys.exit(1)
PY
    then
      pass "debug.descriptor-fields ($desc)"
    else
      fail "debug.descriptor-fields (see $log)"
    fi
    if [[ -n $ajv_js ]]; then
      if node -e '
const Ajv = require(process.argv[1]).default; const fs = require("fs");
const ajv = new Ajv({ strict: false, allErrors: true });
ajv.addSchema(JSON.parse(fs.readFileSync(process.argv[2] + "/common.schema.json", "utf8")));
const v = ajv.compile(JSON.parse(fs.readFileSync(process.argv[2] + "/state-debug.schema.json", "utf8")));
if (!v(JSON.parse(fs.readFileSync(process.argv[3], "utf8")))) { console.log(JSON.stringify(v.errors)); process.exit(1); }' \
        "$ajv_js" "$repo/schema" "$desc" >> "$log" 2>&1; then
        pass "debug.descriptor-schema"
      else
        fail "debug.descriptor-schema (see $log)"
      fi
    else
      say "note: no ajv found (AJV_2020); the schema check was not run"
    fi
    grep -a "debug: developer debugging is on" "$klog" | tail -1 | tee -a "$log" > /dev/null
  else
    fail "debug.descriptor: no kiln/state/debug/*.json within 30 s of the boot"
  fi
  cp "$klog" "$work/kiln-on.log"
  stop_lab
  if compgen -G "$ddir/*.json" > /dev/null; then
    say "note: the descriptor is still there after the stop (no clean exit, or no exit handler ran): stale, detected by its creation time"
  else
    pass "debug.descriptor-removed-at-exit"
  fi
fi

# ---- 2. off ----
say "lab $lab: developer_debugging false"
if run_lab off; then
  sleep 5
  cp "$klog" "$work/kiln-off.log"
  if compgen -G "$ddir/*.json" > /dev/null || [[ -d $ddir ]]; then
    fail "debug.descriptor-off: $ddir exists with the switch off"
  elif grep -aq "debug: developer debugging\|kiln-debug-" "$work/kiln-off.log"; then
    fail "debug.descriptor-off: kiln.log names developer debugging or a control pipe"
  else
    pass "debug.descriptor-off (no descriptor, no pipe line in kiln.log)"
  fi
  stop_lab
fi

if ((fails == 0)); then
  say "PASS debug.descriptor"
else
  say "FAIL debug.descriptor ($fails failed)"
  exit 1
fi
