#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# S2 only: the real per-mod native publisher, not an authored body/rendering acceptance test.
# Run the ENTIRE session under dev31/tools/lab, after win1-native. No managed host or client is needed.
#   dev31/tools/lab bash mods/tests/npc-body-boot-test/lab.sh <native bin dir> <out dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
bin=$(realpath "${1:?native bin directory}")
out=$(realpath -m "${2:?output directory}")
name=dev31-16f
srv=${KILN_LABS:-$HOME/srv/enshrouded/labs}/$name
cd "$repo"
mkdir -p "$out"
# Refuse an existing lab rather than taking over or deleting somebody else's session.
[[ ! -e $srv && ! -e $srv.pfx ]] || { echo "$name already exists" >&2; exit 2; }
created=0
cleanup() {
    if ((created)); then tools/lab/lab.sh stop "$name" || true; tools/lab/lab.sh rm "$name"; fi
}
trap cleanup EXIT
tools/lab/lab.sh new "$name" 31160
created=1
k=$srv/kiln
mkdir -p "$k"/{services,logs,control,state,mods,sigdb/builds/server,regmap/common,regmap/server,locales}
install -m 644 "$bin/dinput8.dll" "$srv/"
install -m 644 "$bin/kiln-core.dll" "$k/"
cp "$bin"/services/*.dll "$k/services/"
cp "$repo/native/core/compat-rules.json" "$k/"
cp "$repo/runtime/locales/"*.json "$k/locales/"
cp "$repo/sigdb/symbols/server.json" "$k/sigdb/"
cp -r "$repo/sigdb/symbols/server.d" "$k/sigdb/"
cp "$repo/sigdb/builds/server/"*.json "$k/sigdb/builds/server/"
cp "$repo/regmap/common/"*.json "$k/regmap/common/"
cp "$repo/regmap/server/"*.json "$k/regmap/server/" 2>/dev/null || true
printf '{"policy":"any","clr":false,"plugins":false,"log_level":"info"}\n' > "$k/kiln.json"
# Made-up mod ids/GUIDs only; no game data is embedded in this fixture.
python3 -I - "$k/mods" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for mod in ("a-boot-first", "b-boot-refused", "c-boot-after"):
    p = root / mod
    (p / "quests").mkdir(parents=True)
    manifest = {"id": mod, "name": mod, "version": "0.1.0", "apiVersion": "0.2",
                "side": "both", "clients": "mod", "advanced": True}
    (p / "kiln.mod.json").write_text(json.dumps(manifest))
    doc = {"format": 1, "knowledge": {"met": {"scope": "world"}}}
    if mod == "b-boot-refused":
        doc["npcs"] = {"keeper": {
            "template": "Townsfolk_Male_villager_01_GuntherGaleforth",
            "clone": {"guid": "9e5b7ad0-5fd0-45ae-a6a4-120335ddf7bb", "name": "Kiln_BootRollback_Keeper"},
            "names": {"en": "Boot rollback fixture"},
            "vendor": {"recipes": [
                {"guid": "1c31a59a-6a56-416c-bdb2-a9edb4c19ef0", "name": "BootFirstRecipe",
                 "base": "UnboundFixtureBase", "displayName": "Unpublished fixture text",
                 "inputs": [{"item": "UnboundFixtureInput"}], "outputs": [{"item": "UnboundFixtureOutput"}]},
                {"guid": "67a27d6c-f6d2-4b34-a42b-127eafdd9e7c", "name": "invalid/name"}
            ]}
        }}
    (p / "quests" / "boot.json").write_text(json.dumps(doc))
PY
checks=0
check() { checks=$((checks + 1)); if "$@"; then echo "PASS $checks"; else echo "FAIL $checks: $*"; exit 1; fi; }
log_has() { /usr/bin/grep -qF -- "$1" "$out/healthy-kiln.log"; }
/mnt/Dongus/kiln-wave/tools/labstart "$name" > "$out/healthy-start.log" 2>&1
tools/lab/lab.sh stop "$name" >> "$out/healthy-start.log" 2>&1
tr -d '\r' < "$k/logs/kiln.log" > "$out/healthy-kiln.log"
tr -d '\r' < "$srv/logs/enshrouded_server.log" > "$out/healthy-game.log"
check /usr/bin/grep -qF 'LoadEcsScene to Run' "$out/healthy-game.log"
check log_has 'bootpack:b-boot-refused:'
check log_has 'debug name'
check log_has 'unpublished definition(s) of bootpack:b-boot-refused taken out'
check log_has 'native: 2 mod(s): 2 new knowledge object(s)'
check log_has 'per-mod boot rollback'
check test ! -f "$k/state/content-refused.json"
check test "$(cat "$k/state/exit-code.txt")" = 0
# The existing fatal-start notice path still stops before any world and preserves the previous save.
rm -rf "$k/mods/b-boot-refused" "$k/mods/c-boot-after"
printf 'compile\n' > "$k/control/knowledge.fail-build"
save_hash() { (cd "$srv" && find savegame -type f -print0 | sort -z | xargs -0 -r sha256sum); }
save_hash > "$out/save-before.sha256"
if /mnt/Dongus/kiln-wave/tools/labstart "$name" > "$out/refused-start.log" 2>&1; then
    echo 'FAIL: the deliberately refused start reached its world'; exit 1
fi
tools/lab/lab.sh stop "$name" >> "$out/refused-start.log" 2>&1
tr -d '\r' < "$k/logs/kiln.log" > "$out/refused-kiln.log"
tr -d '\r' < "$srv/logs/enshrouded_server.log" > "$out/refused-game.log"
save_hash > "$out/save-after.sha256"
for file in content-refused boot progress; do cp "$k/state/$file.json" "$out/$file.json"; done
check test "$(cat "$k/state/exit-code.txt")" = 3
check cmp -s "$out/save-before.sha256" "$out/save-after.sha256"
check test -s "$out/save-before.sha256"
check python3 -I "$repo/mods/tests/content-stop-test/check_stop.py" "$out/content-refused.json" "$out/boot.json" "$out/progress.json" a-boot-first 'lab switch kiln/control/knowledge.fail-build: compile'
check /usr/bin/grep -qF 'native: the start is stopped (pid' "$out/refused-kiln.log"
check /usr/bin/grep -qF 'a-boot-first' "$out/refused-kiln.log"
if /usr/bin/grep -qF 'LoadEcsScene to Run' "$out/refused-game.log"; then echo 'FAIL: a world loaded'; exit 1; fi
checks=$((checks + 1))
echo "native boot lab: $checks checks, 0 failed (two boots; no client)"
