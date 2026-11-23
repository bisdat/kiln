#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# 16f lab on the product path: a dedicated-server lab with Kiln as deploy-server.sh lays it out (natives from <native bin dir>, the managed
# host and the test mods from $KILN_OUT), no player. Run the WHOLE session under dev31/tools/lab, from the worktree's root:
#
#   KILN_OUT=<build> [SLOT=dev31/tools/slot] dev31/tools/lab bash mods/tests/npc-body-test/lab.sh bodies <native bin dir> <out dir>
#   KILN_OUT=<build> dev31/tools/lab bash mods/tests/npc-body-test/lab.sh suites <native bin dir> <out dir>
#
# $KILN_OUT: host/ (Kiln.Host, the services, Kiln.Game), cmake/bin (what deploy-server.sh installs first) and mods/<id> built by
# dotnet build (npc-body-test; for "suites" quest-native-test and blocks-npc-test, its assets cooked by build-assets.py).
#
# bodies: npc-body-test and npc-body-broken (the same body under another id; its glTF is replaced after its cook: a stale cook), both
#   cooked in the lab's mod folders with the lab server's files (what `kiln cook <mod> --game <server>` runs), lab_expect.py's
#   expectations, then three starts: (1) the build; (2) a restart, with handshake.py's fake clients at the join check; (3) npc-body-test
#   taken out. Checks kiln.log, the body ledger and the mod's PASS/FAIL lines; prints "npc body lab: N checks, 0 failed".
# suites: quest-native-test and blocks-npc-test (force-ready switch), two starts; prints each start's "done" lines, for comparing builds.
# The lab (dev31-16f, UDP 31160; kiln.net lab port 31161) is removed at the end (KEEP_LAB=1 keeps it).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
mode=${1:?bodies|suites}
bin=$(realpath "${2:?native bin directory}")
out=$(realpath -m "${3:?output directory}")
: "${KILN_OUT:?KILN_OUT: the build (host/, cmake/bin, mods/)}"
SLOT=${SLOT:-}
name=dev31-16f
port=${LAB_PORT:-31160}
srv=${KILN_LABS:-$HOME/srv/enshrouded/labs}/$name
k=$srv/kiln
cd "$repo" || exit 2
[[ ! -e $srv && ! -e $srv.pfx ]] || { echo "$name already exists" >&2; exit 2; }
rm -rf "$out" && mkdir -p "$out"
created=0
cleanup() {
    if ((created)); then
        tools/lab/lab.sh stop "$name" >/dev/null 2>&1 || true
        [[ ${KEEP_LAB:-0} == 1 ]] || tools/lab/lab.sh rm "$name" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
checks=0 failed=0
check() {
    local what=$1; shift
    checks=$((checks + 1))
    if "$@"; then echo "PASS $what"; else echo "FAIL $what"; failed=$((failed + 1)); fi
}
has() { /usr/bin/grep -qE -- "$2" "$1"; }

tools/lab/lab.sh new "$name" "$port" > "$out/lab.log" 2>&1 || { cat "$out/lab.log"; exit 1; }
created=1
tools/deploy-server.sh "$srv" --no-build --policy any > "$out/deploy.log" 2>&1 || { echo "deploy failed (deploy.log)"; exit 1; }
# the natives under test over what deploy laid out; no native plugins (nothing here needs one)
install -m 644 "$bin/dinput8.dll" "$srv/"
install -m 644 "$bin/kiln-core.dll" "$k/"
rsync -a --delete --include='*.dll' --include='*.pdb' --exclude='*' "$bin/services/" "$k/services/"
rm -rf "$k/plugins" "$k/mods" && mkdir -p "$k/plugins" "$k/mods"
sha256sum "$bin/services/kiln-quests.dll" | tee "$out/natives.txt"
mod() { mkdir -p "$k/mods/$1" && rsync -a --exclude='Kiln.*' --exclude='*.deps.json' --exclude='*.runtimeconfig.json' "$KILN_OUT/mods/$1/" "$k/mods/$1/"; }

# one start: the logs of it in $out/<run>; waits for the given kiln.log lines (or the lab's end)
start() {
    local run=$1; shift
    local r=$out/$run
    mkdir -p "$r"
    rm -f "$k/logs/kiln.log"
    /mnt/Dongus/kiln-wave/tools/labstart "$name" > "$r/start.log" 2>&1
    echo "labstart rc=$?" >> "$r/start.log"
    local want
    for want in "$@"; do
        for _ in $(seq 1 150); do
            /usr/bin/grep -qF -- "$want" "$k/logs/kiln.log" 2>/dev/null && break
            [[ -n $(tools/lab/lab.sh pid "$name" 2>/dev/null) ]] || break
            sleep 2
        done
    done
}
collect() {
    local r=$out/$1
    tools/lab/lab.sh stop "$name" >> "$r/start.log" 2>&1
    tr -d '\r' < "$k/logs/kiln.log" > "$r/kiln.log" 2>/dev/null
    tr -d '\r' < "$srv/logs/enshrouded_server.log" > "$r/game.log" 2>/dev/null
    cp "$k/data/kiln-quests/native/npc-bodies.ledger.json" "$r/ledger.json" 2>/dev/null || echo '{}' > "$r/ledger.json"
    /usr/bin/grep -E "npc-body-test|NPC bod|npc-body-broken|bootpack|native: " "$r/kiln.log" | cut -c1-400 > "$r/lines.txt"
}
ledger() {  # ledger <run> <python condition on rows>
    python3 -I - "$out/$1/ledger.json" "$2" <<'PY'
import json, re, sys
d = json.load(open(sys.argv[1]))
rows = {f"{b['mod']}:{b['npc']}": b for b in d.get("bodies", [])}
hex64 = lambda s: re.fullmatch(r"[0-9a-f]{64}", s or "") is not None
sys.exit(0 if eval(sys.argv[2], {"rows": rows, "hex64": hex64}) else 1)
PY
}

if [[ $mode == suites ]]; then
    mod quest-native-test
    mod blocks-npc-test
    mkdir -p "$k/data/kiln.creatures" && : > "$k/data/kiln.creatures/force-ready"
    for run in run1 run2; do
        start "$run" "quest-native-test: done:" "blocks-npc-test done:"
        collect "$run"
        echo "$run: $(/usr/bin/grep -hoE "quest-native-test: done: [0-9]+ PASS, [0-9]+ FAIL|blocks-npc-test done: [0-9]+ PASS, [0-9]+ FAIL" "$out/$run/kiln.log" | tr '\n' ';')"
        /usr/bin/grep -hE "(quest-native-test|blocks-npc-test): FAIL" "$out/$run/kiln.log" | cut -c1-300
    done
    exit 0
fi
[[ $mode == bodies ]] || { echo "mode: bodies or suites" >&2; exit 2; }

# ---- the mods, cooked here as a server's own cook does ----
mod npc-body-test
mkdir -p "$k/mods/npc-body-test/models"
python3 -I tools/modelcook/samples/rigged_figure.py "$k/mods/npc-body-test/models/figure.glb" > /dev/null
b=$k/mods/npc-body-broken
mkdir -p "$b/blocks" "$b/models" "$b/assets"
cp "$k/mods/npc-body-test/blocks/npc-body.json" "$b/blocks/"
cp "$k/mods/npc-body-test/models/figure.glb" "$b/models/"
cp "$k/mods/npc-body-test/assets/cook.json" "$b/assets/"
cat > "$b/kiln.mod.json" <<'JSON'
{ "id": "npc-body-broken", "name": "Broken NPC body neighbour (16f lab)", "version": "0.1.0", "apiVersion": "0.2", "side": "both", "clients": "mod",
  "description": "npc-body-test's body under another id, whose glTF was replaced after its cook: the boot pack refuses this mod alone." }
JSON
for m in npc-body-broken npc-body-test; do
    KILN_COOK_RECEIPT=1 $SLOT python3 -I tools/animcook/animcook.py --game "$srv" recipe "$k/mods/$m/assets/cook.json" --update-manifest > "$out/cook-$m.log" 2>&1
    check "cook $m with the server's files" test $? = 0
done
check "the server cook marks the model skippedOnServer and binds 27 motions" python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1]))
rows = [e for e in d["entries"] if e.get("type") == "keen::RenderModel"]
sys.exit(0 if len(rows) == 1 and rows[0].get("skippedOnServer") and len(d["bodies"]["figure"]["bindings"]) == 27 and
         {e["sourceHash"] for e in d["entries"]} == {d["sourceHash"]} else 1)' "$k/mods/npc-body-test/assets/cooked/npc-bodies.v1.json"
python3 -I tools/modelcook/samples/rigged_figure.py "$b/models/figure.glb" --lod1 > /dev/null  # the stale cook: the author's glTF changed since
$SLOT python3 -I "$here/lab_expect.py" "$srv" "$k/mods/npc-body-test" "$repo" > "$k/mods/npc-body-test/lab-expected.json" 2> "$out/expect.log"
check "lab_expect: the clone ids are no game resources" test $? = 0
cp "$k/mods/npc-body-test/lab-expected.json" "$out/"
mkdir -p "$k/config" && printf '{ "labPort": %d }\n' $((port + 1)) > "$k/config/network.json"
clone=$(python3 -I -c 'import uuid; print(uuid.uuid5(uuid.UUID("6f6d2a44-53c1-5c52-9d51-0a3c6e1f2b77"), "npc-body-test:npc:figure"))')

# ---- start 1: the build ----
start run1 "npc-body-test done:"
collect run1
r=$out/run1
check "run1: the world loaded" has "$r/game.log" 'LoadEcsScene to Run'
check "run1: kiln.log: the body is ready with its template and a 64-hex closure" has "$r/kiln.log" "native: NPC body npc-body-test:figure: ready \(template $clone, closure [0-9a-f]{64}\)"
check "run1: kiln.log: the broken neighbour failed with its reason" has "$r/kiln.log" 'native: NPC body npc-body-broken:figure: failed: bootpack:npc-body-broken: npcs\.figure: assets/cooked/npc-bodies\.v1\.json: models/figure\.glb changed after the last cook; cook the mod again'
check "run1: ledger: ready with a 64-hex closure, the neighbour failed" ledger run1 \
    'rows["npc-body-test:figure"]["state"] == "ready" and hex64(rows["npc-body-test:figure"]["closure"]) and rows["npc-body-broken:figure"]["state"] == "failed"'
check "run1: the start was not stopped" bash -c "! /usr/bin/grep -qF 'native: the start is stopped' '$r/kiln.log'"
check "run1: npc-body-test: no FAIL" bash -c "! /usr/bin/grep -qF 'npc-body-test: FAIL' '$r/kiln.log'"
check "run1: npc-body-test: done with 0 FAIL" has "$r/kiln.log" 'npc-body-test done: [0-9]+ PASS, 0 FAIL'
closure1=$(/usr/bin/grep -oE "npc-body-test:figure: ready \(template [0-9a-f-]+, closure [0-9a-f]{64}\)" "$r/kiln.log" | /usr/bin/grep -oE '[0-9a-f]{64}')

# ---- start 2: a restart, and the join check ----
start run2 "npc-body-test done:"
python3 -I "$here/handshake.py" --port $((port + 1)) --lab "$srv" > "$out/run2-handshake.txt" 2>&1
check "run2: handshake.py's fake clients at the join check" test $? = 0
collect run2
r=$out/run2
check "run2: the same closure after a restart" has "$r/kiln.log" "native: NPC body npc-body-test:figure: ready \(template $clone, closure ${closure1:-none}\)"
check "run2: npc-body-test: no FAIL" bash -c "! /usr/bin/grep -qF 'npc-body-test: FAIL' '$r/kiln.log'"
check "run2: npc-body-test: done with 0 FAIL, the restart checks among them" has "$r/kiln.log" 'npc-body-test: PASS after a restart the npcId is the same'
check "run2: npc-body-test done" has "$r/kiln.log" 'npc-body-test done: [0-9]+ PASS, 0 FAIL'

# ---- start 3: npc-body-test taken out ----
rm -rf "$k/mods/npc-body-test"
start run3 "NPC body npc-body-broken:figure: failed"
sleep 10
collect run3
r=$out/run3
check "run3: the world loaded" has "$r/game.log" 'LoadEcsScene to Run'
check "run3: kiln.log: the body taken out is reported removed" has "$r/kiln.log" 'native: NPC body npc-body-test:figure: removed: no longer built at this start'
check "run3: ledger: no row of the removed mod, the neighbour still failed" ledger run3 \
    '"npc-body-test:figure" not in rows and rows["npc-body-broken:figure"]["state"] == "failed"'
check "run3: the start was not stopped" bash -c "! /usr/bin/grep -qF 'native: the start is stopped' '$r/kiln.log'"

for run in run1 run2; do
    echo "$run: $(/usr/bin/grep -hoE 'npc-body-test done: [0-9]+ PASS, [0-9]+ FAIL' "$out/$run/kiln.log")"
done
cat "$out/run2-handshake.txt"
echo "npc body lab: $checks checks, $failed failed (three starts; no client)"
exit $((failed > 0))
