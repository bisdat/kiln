#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The hash a server advertises for another engine's mod (reference implementation, 0.2.0-dev.29.18).

    python3 engine_identity.py --write   rebuild cases.json: the trees below and the hash this reference gives each
    python3 engine_identity.py --check   materialize cases.json's trees and compare with their "expect"

Kiln.Network (managed/services/Kiln.Network/AdvertiseIdentity.cs, NetAdvertise.EngineHash) and kiln-manage
(crates/kiln-manage/src/modpack.rs, engine_identity, through modpack::required) read cases.json in their tests and must
give each case its "expect". Change the rule in all three, then --write and review the diff.

The rule, for a mod folder DIR of kind KIND (eml, shroudforge, set, greeneye, flame, shroudtopia, enshwick) in a game
or server folder ROOT:

  1. Kiln's copy. A receipt entry outside the folder says the launcher put it there:
     - server sync: DIR/.kiln-source.json is an object with "format": 1 and a "library" {kind, id, version, hash} whose
       kind is KIND and id is DIR's name; an entry of ROOT/kiln/state/server-library.json (an object keyed by path) has
       the same "library", a key whose last two names are DIR's parent and DIR's name, and a "files" object;
     - activation: DIR/.kiln-library.json is an object with "format": 1, kind KIND, id DIR's name, version and hash; an
       entry of ROOT/kiln/state/library-placements.json (an array) has a "marker" equal to it (format 1), a "path" whose
       last two names are DIR's parent and DIR's name, and a "files" object ("game_folder": true, a copy in <game>/mods
       for a DLL that reads it there).
     Names compare with the case of ASCII letters ignored; either slash separates them. Sync entries first, then
     activation entries. An entry is current when every file it lists (a key not ending in "/" with a string value) that
     is not left out (3) and is no log (4) is a regular file in DIR (no link) whose sha256 is the listed one (case of the
     hex digits ignored). The first current entry wins: the advertised hash is its library hash.
  2. Any other folder: kdh1 (schema/fixtures/dirhash) of DIR's files without logs (4) and the files left out (3, not
     as a game-folder copy).
  3. Left out (what may change while the mod runs): at the top of the folder, "extended.mod.json" of a shroudforge or
     eml mod and the kind's settings files (eml: config.txt, config.ini; set: settings.json); in a game-folder copy also
     any file with a settings extension (txt ini cfg json toml yaml yml) that is not mod.json, .kiln-library.json,
     native-plugin.ini or below src/. Names compare with the case of ASCII letters ignored.
  4. A log: the last name's extension is "log" (ASCII case ignored; a name starting with its only dot has none).

A JSON file the rule reads is at most 16 MiB of strict JSON; one that is absent or unreadable says nothing.
"""
import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "dirhash"))
import dirhash  # noqa: E402  (the kdh1 reference)

SETTINGS_FILES = {"eml": ["config.txt", "config.ini"], "set": ["settings.json"]}
SETTINGS_EXTENSIONS = {"txt", "ini", "cfg", "json", "toml", "yaml", "yml"}
ROOT = "{root}"  # receipt paths below it are the folder's real path in a test


def lower(s: str) -> str:
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


def extension(rel: str) -> str:
    name = rel.replace("\\", "/").rsplit("/", 1)[-1]
    if "." not in name:
        return ""
    stem, x = name.rsplit(".", 1)
    return "" if stem == "" else lower(x)


def is_log(rel: str) -> bool:
    return extension(rel) == "log"


def game_settings_file(rel: str) -> bool:
    low = lower(rel)
    return not (low in ("mod.json", ".kiln-library.json", "native-plugin.ini") or low.startswith("src/")) \
        and extension(low) in SETTINGS_EXTENSIONS


def left_out(kind: str, rel: str, game_folder: bool) -> bool:
    top = "/" not in rel
    return (top and kind in ("shroudforge", "eml") and lower(rel) == "extended.mod.json") \
        or (top and lower(rel) in SETTINGS_FILES.get(kind, [])) \
        or (game_folder and game_settings_file(rel))


def read_json(path: Path):
    try:
        if not path.is_file() or path.stat().st_size > 16 << 20:
            return None
        return json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, ValueError):
        return None


def library(v):
    if not isinstance(v, dict):
        return None
    vals = [v.get(k) for k in ("kind", "id", "version", "hash")]
    return tuple(vals) if all(isinstance(x, str) for x in vals) else None


def format1(v) -> bool:
    return isinstance(v, dict) and type(v.get("format")) is int and v["format"] == 1


def leaf_names(path: str, name: str, parent: str) -> bool:
    parts = path.replace("\\", "/").rstrip("/").split("/")
    return len(parts) >= 2 and lower(parts[-1]) == lower(name) and lower(parts[-2]) == lower(parent)


def copies(root: Path, kind: str, d: Path):
    """(library hash, files, game_folder) for each receipt entry that names d with its marker: sync first."""
    out = []
    name, parent = d.name, d.parent.name
    state = root / "kiln" / "state"
    s = read_json(d / ".kiln-source.json")
    lib = library(s.get("library")) if format1(s) else None
    synced = read_json(state / "server-library.json") if lib and lib[0] == kind and lib[1] == name else None
    if isinstance(synced, dict):
        for path, e in synced.items():
            if isinstance(e, dict) and library(e.get("library")) == lib and leaf_names(path, name, parent) and isinstance(e.get("files"), dict):
                out.append((lib[3], e["files"], False))
    m = read_json(d / ".kiln-library.json")
    want = library(m) if format1(m) else None
    placed = read_json(state / "library-placements.json") if want and want[0] == kind and want[1] == name else None
    if isinstance(placed, list):
        for r in placed:
            if not isinstance(r, dict):
                continue
            marker = r.get("marker")
            if format1(marker) and library(marker) == want and isinstance(r.get("path"), str) and leaf_names(r["path"], name, parent) \
                    and isinstance(r.get("files"), dict):
                out.append((want[3], r["files"], r.get("game_folder", r.get("gameFolder")) is True))
    return out


def current(kind: str, d: Path, files: dict, game_folder: bool) -> bool:
    for rel, sha in files.items():
        if rel.endswith("/") or not isinstance(sha, str) or left_out(kind, rel, game_folder) or is_log(rel):
            continue
        p = d / rel
        try:
            if not stat.S_ISREG(os.lstat(p).st_mode):
                return False
        except OSError:
            return False
        if hashlib.sha256(p.read_bytes()).hexdigest() != lower(sha):
            return False
    return True


def advertised(root: Path, kind: str, d: Path) -> str:
    for lib_hash, files, game_folder in copies(root, kind, d):
        if current(kind, d, files, game_folder):
            return lib_hash
    h = hashlib.sha256()
    for key, path in dirhash.files(d):
        rel = key.decode("utf-8")
        if is_log(rel) or left_out(kind, rel, False):
            continue
        data = path.read_bytes()
        h.update(key + b"\0" + str(len(data)).encode() + b"\0" + hashlib.sha256(data).hexdigest().encode() + b"\n")
    return "kdh1:" + h.hexdigest()


# ---------------------------------------------------------------------------------------------------------- the cases


def text(s: str) -> dict:
    return {"text": s}


def binary(b: bytes) -> dict:
    return {"hex": b.hex()}


def content(v: dict) -> bytes:
    """A file's bytes: "text" (UTF-8, line ends as written) or "hex"."""
    return v["text"].encode("utf-8") if "text" in v else bytes.fromhex(v["hex"])


def sha(v: dict) -> str:
    return hashlib.sha256(content(v)).hexdigest()


def extended(enabled: bool, speed: int) -> dict:
    return text(json.dumps({"schemaVersion": 1, "enabled": enabled, "targets": ["client", "server"],
                            "settings": {"speed": {"label": "Speed", "control": "slider", "min": 1, "max": 10, "value": speed}}}, indent=2) + "\n")


SF_ID = "unifiedemltweaks"
SF = {  # a ShroudForge package shaped like Unified EML Tweaks 2.1.1 (its native-plugin.ini has Windows line ends)
    "mod.json": text('{\n  "id": "unifiedemltweaks",\n  "name": "Unified EML Tweaks",\n  "version": "2.1.1"\n}\n'),
    "extended.mod.json": extended(True, 3),
    "native-plugin.ini": text("[Plugin]\r\nEnabled=1\r\nDll=bin/ut_native.dll\r\n"),
    "bin/ut_native.dll": binary(b"MZ\x90\x00\x03\x00\x00\x00 not run " + bytes(range(256))),
    "src/mod.lua": text("-- settings come from ShroudForge\r\nreturn {}\r\n"),
    "locales/pl.json": text('{"speed": "Szybko\u015b\u0107"}\n'),
    "reference/ut-reference.txt": text("Unified Tweaks reference\n"),
    "icon.svg": text("<svg xmlns='http://www.w3.org/2000/svg'/>\n"),
}
ODD = {  # nested folders, odd names (spaces, a decomposed umlaut, upper case), Windows and Unix line ends
    "data/a/b/c/table.bin": binary(bytes(range(255, -1, -1))),
    "data/Read Me.txt": text("line one\r\nline two\n"),
    unicodedata.normalize("NFD", "data/\u00dcbersicht.lua"): text("return 1\n"),
    "data/caf\u00e9 (copy).json": text("{}\r\n"),
    "src/crlf.lua": text("local a = 1\r\nreturn a\r\n"),
    ".hidden/skip.txt": text("never counted\n"),
}
RUN = {  # what the mod and the player do while it runs: logs, a status file, a setting changed
    "bin/ut_native.log": text("started\nmore\n"),
    "logs/Run.LOG": text("run\n"),
    "bin/native-status.txt": text("tick 2\n"),
    "extended.mod.json": extended(True, 8),
}
LIBRARY = "kdh1:" + hashlib.sha256(b"the Library entry of unifiedemltweaks 2.1.1").hexdigest()
LIBRARY_OTHER = "kdh1:" + hashlib.sha256(b"the Library entry of unifiedemltweaks 2.1.0").hexdigest()


def folders(files: dict) -> list:
    out = set()
    for rel in files:
        parts = rel.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            out.add("/".join(parts[:i]) + "/")
    return sorted(out)


def listed(files: dict, upper: bool = False) -> dict:
    """A receipt's file list of the files placed: folders "", files their sha256 (extended.mod.json by a hash of its own,
    which no side compares)."""
    out = {d: "" for d in folders(files)}
    for rel, v in files.items():
        h = hashlib.sha256(b"extended.mod.json without its switch and settings").hexdigest() if rel == "extended.mod.json" else sha(v)
        out[rel] = h.upper() if upper else h
    return dict(sorted(out.items()))


def marker(kind: str, mid: str, version: str, h: str) -> dict:
    return {"format": 1, "kind": kind, "id": mid, "version": version, "hash": h}


def placed_case(name, files, kind="shroudforge", folder=f"mods/{SF_ID}", mid=SF_ID, h=LIBRARY, run=None, receipts=None, **extra):
    """Kiln's copy placed by activation: the marker in the folder, the receipt naming it by a Steam Deck's path."""
    m = marker(kind, mid, "2.1.1", h)
    tree = dict(files)
    tree[".kiln-library.json"] = text(json.dumps(m))
    if receipts is None:
        receipts = [{"path": f"/run/media/deck/GF8S5/steamapps/common/Enshrouded/{folder}", "marker": m, "files": listed(files)}]
    tree.update(run or {})
    return {"name": name, "kind": kind, "folder": folder, "files": tree,
            "state": {"library-placements.json": receipts}, **extra}


def synced_case(name, files, kind="shroudforge", folder=f"mods/{SF_ID}", mid=SF_ID, h=LIBRARY, run=None, with_files=True, **extra):
    """Kiln's copy synced to a server: .kiln-source.json in the folder, the server's receipt naming it by the launcher's path."""
    lib = {"kind": kind, "id": mid, "version": "2.1.1", "hash": h}
    tree = dict(files)
    tree[".kiln-source.json"] = text(json.dumps({"format": 1, "library": lib, "synced": "2026-11-18T12:00:00Z", "by": "kiln-launcher"}))
    entry = {"library": lib, "payload": "0" * 64}
    if with_files:
        entry["files"] = listed(files)
    tree.update(run or {})
    return {"name": name, "kind": kind, "folder": folder, "files": tree,
            "state": {"server-library.json": {f"/home/me/servers/isles/{folder}": entry}}, **extra}


def own_case(name, files, kind="shroudforge", folder=f"mods/{SF_ID}", run=None, **extra):
    tree = dict(files)
    tree.update(run or {})
    return {"name": name, "kind": kind, "folder": folder, "files": tree, "state": {}, **extra}


def without(files: dict, rel: str) -> dict:
    return {k: v for k, v in files.items() if k != rel}


SET_ID = "faster-boats"
SET = {"mod.json": text('{"id": "faster-boats", "name": "Faster Boats", "version": "2.1.1", "clients": "mod"}\n'),
       "init.lua": text("return {}\n"), "settings.json": text('{"speed": 2}\n')}
EML_ID = "bigger-stacks"
EML = {"mod.json": text('{"id": "bigger-stacks", "name": "Bigger Stacks", "version": "2.1.1"}\n'),
       "src/mod.lua": text("return {}\n"), "config.txt": text("stack=100\n"), "data/table.ini": text("[a]\nb=1\n")}


def cases():
    sf_odd = {**SF, **ODD}
    stale = listed(SF)
    stale["src/mod.lua"] = "0" * 64
    m = marker("shroudforge", SF_ID, "2.1.1", LIBRARY)
    both = placed_case("sf-both-markers", SF, h=LIBRARY_OTHER)
    syn = synced_case("x", SF)
    both["files"][".kiln-source.json"] = syn["files"][".kiln-source.json"]
    both["state"].update(syn["state"])
    both["library"] = LIBRARY  # server sync is read first
    eml_game = placed_case("eml-game-folder-copy", EML, kind="eml", folder=f"kiln/eml/mods/{EML_ID}", mid=EML_ID,
                           run={"data/table.ini": text("[a]\nb=2\n")}, library=LIBRARY)
    eml_game["state"]["library-placements.json"][0]["game_folder"] = True
    eml_not_game = placed_case("eml-copy-table-changed", EML, kind="eml", folder=f"kiln/eml/mods/{EML_ID}", mid=EML_ID,
                               run={"data/table.ini": text("[a]\nb=2\n")}, library=None)
    return [
        placed_case("sf-placed", SF, library=LIBRARY),
        placed_case("sf-placed-run", SF, run=RUN, library=LIBRARY),
        # switched off in ShroudForge's file: not advertised at all (it does not run), whatever its hash
        placed_case("sf-placed-switched-off", SF, run={"extended.mod.json": extended(False, 3)}, library=LIBRARY, listed=False),
        placed_case("sf-placed-exact-path", SF, receipts=[{"path": f"{ROOT}/mods/{SF_ID}", "marker": m, "files": listed(SF)}], library=LIBRARY),
        placed_case("sf-placed-lua-changed", SF, run={"src/mod.lua": text("return { edited = true }\n")}, library=None),
        placed_case("sf-placed-dll-updated", SF, run={**RUN, "bin/ut_native.dll": binary(b"MZ a newer build")}, library=None),
        placed_case("sf-placed-ini-changed", SF, run={"native-plugin.ini": text("[Plugin]\r\nEnabled=1\r\nDll=bin/ut_native_v2.dll\r\n")}, library=None),
        placed_case("sf-placed-file-gone", without(SF, "locales/pl.json"), receipts=[
            {"path": f"/run/media/deck/GF8S5/steamapps/common/Enshrouded/mods/{SF_ID}", "marker": m, "files": listed(SF)}], library=None),
        placed_case("sf-placed-dll-added", SF, run={"bin/ut_extra.dll": binary(b"MZ added by the mod")}, library=LIBRARY),
        placed_case("sf-placed-ships-a-log", {**SF, "bin/readme.log": text("shipped\n")},
                    run={"bin/readme.log": text("shipped\nappended\n")}, library=LIBRARY),
        placed_case("sf-placed-odd-names", sf_odd, run={**RUN, "data/CRASH.LOG": text("x\n")}, library=LIBRARY),
        placed_case("sf-placed-receipt-upper-case", SF, receipts=[
            {"path": f"D:\\steamapps\\common\\Enshrouded\\MODS\\{SF_ID}", "marker": m, "files": listed(SF, upper=True)}], library=LIBRARY),
        placed_case("sf-placed-stale-and-current", SF, receipts=[
            {"path": f"/old/Enshrouded/mods/{SF_ID}", "marker": m, "files": stale},
            {"path": f"/run/media/deck/GF8S5/steamapps/common/Enshrouded/mods/{SF_ID}", "marker": m, "files": listed(SF)}], library=LIBRARY),
        placed_case("sf-placed-old-receipt", SF, receipts=[
            {"path": f"/run/media/deck/GF8S5/steamapps/common/Enshrouded/mods/{SF_ID}", "marker": m}], library=None),
        placed_case("sf-forged-marker", SF, receipts=[], library=None),
        placed_case("sf-receipt-elsewhere", SF, receipts=[
            {"path": f"/elsewhere/shroudforge-mods/{SF_ID}", "marker": m, "files": listed(SF)}], library=None),
        placed_case("sf-receipt-other-version", SF, receipts=[
            {"path": f"/run/media/deck/GF8S5/steamapps/common/Enshrouded/mods/{SF_ID}",
             "marker": marker("shroudforge", SF_ID, "2.1.0", LIBRARY_OTHER), "files": listed(SF)}], library=None),
        synced_case("sf-synced", SF, library=LIBRARY),
        synced_case("sf-synced-run", SF, run=RUN, library=LIBRARY),
        synced_case("sf-synced-old-receipt", SF, with_files=False, library=None),
        both,
        own_case("sf-own", SF, library=None, same="sf-own-run"),
        own_case("sf-own-run", SF, run={"bin/ut_native.log": text("started\n"), "extended.mod.json": extended(True, 8)}, library=None),
        own_case("sf-own-status-file", SF, run={"bin/native-status.txt": text("tick 1\n")}, library=None, differs="sf-own"),
        own_case("sf-own-odd-names", sf_odd, library=None, same="sf-own-odd-names-run"),
        own_case("sf-own-odd-names-run", sf_odd, run={"data/CRASH.LOG": text("x\n"), "extended.mod.json": extended(True, 1)}, library=None),
        placed_case("set-placed-settings", SET, kind="set", folder=f"shrouded_engine_tweaks/mods/{SET_ID}", mid=SET_ID,
                    run={"settings.json": text('{"speed": 5}\n')}, library=LIBRARY),
        own_case("set-own", SET, kind="set", folder=f"shrouded_engine_tweaks/mods/{SET_ID}", library=None, same="set-own-settings"),
        own_case("set-own-settings", SET, kind="set", folder=f"shrouded_engine_tweaks/mods/{SET_ID}",
                 run={"settings.json": text('{"speed": 5}\n')}, library=None),
        synced_case("eml-synced-config", EML, kind="eml", folder=f"kiln/eml/mods/{EML_ID}", mid=EML_ID,
                    run={"config.txt": text("stack=999\n"), "CONFIG.INI": text("[x]\n")}, library=LIBRARY),
        eml_game,
        eml_not_game,
        own_case("eml-own", EML, kind="eml", folder=f"kiln/eml/mods/{EML_ID}", library=None, same="eml-own-config"),
        own_case("eml-own-config", EML, kind="eml", folder=f"kiln/eml/mods/{EML_ID}", run={"config.txt": text("stack=5\n")}, library=None),
    ]


# ---------------------------------------------------------------------------------------------------------- materialize


def materialize(case: dict, root: Path) -> Path:
    """Writes the case's tree into root (receipt paths below {root} name root's folder) and returns the mod folder."""
    d = root / case["folder"]
    for rel, v in case["files"].items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(content(v))
    state = root / "kiln" / "state"
    for name, value in case["state"].items():
        state.mkdir(parents=True, exist_ok=True)
        (state / name).write_text(json.dumps(rooted(value, str(root))))
    return d


def rooted(v, root: str):
    """v with "{root}" in its strings (keys too) replaced by root."""
    if isinstance(v, str):
        return v.replace(ROOT, root)
    if isinstance(v, list):
        return [rooted(x, root) for x in v]
    if isinstance(v, dict):
        return {rooted(k, root): rooted(x, root) for k, x in v.items()}
    return v


def compute(case: dict) -> str:
    with tempfile.TemporaryDirectory(prefix="engine-identity-") as t:
        root = Path(t)
        return advertised(root, case["kind"], materialize(case, root))


def relations(by_name: dict) -> list:
    bad = []
    for c in by_name.values():
        if c.get("library") and c["expect"] != c["library"]:
            bad.append(f"{c['name']}: expected the Library entry {c['library']}, got {c['expect']}")
        if "library" in c and c["library"] is None and c["expect"] in (LIBRARY, LIBRARY_OTHER):
            bad.append(f"{c['name']}: must not be the Library entry's hash")
        if c.get("same") and by_name[c["same"]]["expect"] != c["expect"]:
            bad.append(f"{c['name']}: must be the same as {c['same']}")
        if c.get("differs") and by_name[c["differs"]]["expect"] == c["expect"]:
            bad.append(f"{c['name']}: must differ from {c['differs']}")
    return bad


def main(argv) -> int:
    out = HERE / "cases.json"
    if argv[1:] == ["--write"]:
        all_cases = cases()
        for c in all_cases:
            c["expect"] = compute(c)
        bad = relations({c["name"]: c for c in all_cases})
        for b in bad:
            print(f"FAIL {b}")
        if bad:
            return 1
        doc = {"format": 1, "about": "engine_identity.py writes this file: see its head for the rule", "root": ROOT, "cases": all_cases}
        out.write_text(json.dumps(doc, indent=1, ensure_ascii=True) + "\n")
        print(f"wrote {len(all_cases)} cases")
        return 0
    if argv[1:] == ["--check"]:
        doc = json.loads(out.read_text())
        bad = [f"{c['name']}: expected {c['expect']}, got {got}" for c in doc["cases"] if (got := compute(c)) != c["expect"]]
        bad += relations({c["name"]: c for c in doc["cases"]})
        for b in bad:
            print(f"FAIL {b}")
        print("ok" if not bad else f"{len(bad)} failed")
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
