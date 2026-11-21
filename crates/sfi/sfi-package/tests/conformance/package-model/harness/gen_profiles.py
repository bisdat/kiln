# SPDX-License-Identifier: MIT
# mod-profiles/*.json differential cases.
import json, sys
d = lambda v: json.dumps(v, separators=(",", ":"), ensure_ascii=False)
DEL = object()
P0 = {"schemaVersion": 1, "id": "p1", "packageDirectory": "pkg", "name": "Profile"}
def P(**kw):
    out = dict(P0)
    for k, v in kw.items():
        k = {"modId": "modId", "packageDirectory": "packageDirectory"}.get(k, k)
        if v is DEL:
            out.pop(k, None)
        else:
            out[k] = v
    return out
MOD = {"id": "example", "name": "Example", "version": "1.0.0", "capabilities": ["runtime"]}
EXT = {"schemaVersion": 1, "enabled": True, "settings": {"flag": True, "level": {"value": 1, "label": "Level"}}}
PKG = {"mod.json": d(MOD), "extended.mod.json": d(EXT)}
cases = []
def case(cid, profiles, *, package=None, selections=None, directory=None):
    c = {"id": cid, "profiles": dict(profiles)}
    if package is not None:
        c["package"] = package
    if selections is not None:
        c["selections"] = selections
    if directory is not None:
        c["directory"] = directory
    cases.append(c)
def one(cid, profile, **kw):
    case(cid, {"a.json": d(profile)}, **kw)
one("ok.minimal", P())
for i, v in enumerate([1, 1.0]):
    one(f"schemaVersion.ok.{i}", P(schemaVersion=v))
for i, v in enumerate([2, 0, "1", None, DEL, -1, 1.5]):
    one(f"schemaVersion.bad.{i}", P(schemaVersion=v))
for i, v in enumerate(["x", "é" * 64, "i" * 128, "nonE", "with space", "../x"]):
    one(f"id.ok.{i}", P(id=v))
for i, v in enumerate(["", " ", "none", "é" * 65, "i" * 129, 1, None, DEL]):
    one(f"id.bad.{i}", P(id=v))
for i, v in enumerate(["", "example", "Example", "x y"]):
    one(f"modId.ok.{i}", P(modId=v))
for i, v in enumerate([1, None]):
    one(f"modId.bad.{i}", P(modId=v))
for i, v in enumerate(["pkg", "PKG", "a.b", "-", "dir name"]):
    one(f"packageDirectory.ok.{i}", P(packageDirectory=v))
for i, v in enumerate(["", ".", "..", "a/b", "a\\b", "c:", "/abs", "a/", "./a", 1, DEL]):
    one(f"packageDirectory.bad.{i}", P(packageDirectory=v))
for i, v in enumerate(["N", "n" * 160, "é" * 80]):
    one(f"name.ok.{i}", P(name=v))
for i, v in enumerate(["", "  ", "n" * 161, "é" * 81, 1, DEL]):
    one(f"name.bad.{i}", P(name=v))
for i, v in enumerate(["client", "server", "both", DEL]):
    one(f"target.ok.{i}", P(target=v))
for i, v in enumerate(["Both", "all", None, 1]):
    one(f"target.bad.{i}", P(target=v))
for i, v in enumerate([{}, {"description": "x"}, {"capabilities": ["patch"]}, None]):
    one(f"modJson.ok.{i}", P(modJson=v))
for i, v in enumerate([[], "x", 1, {"id": "x"}, {"version": "2.0.0"}, {"capabilities": ["native"]}, {"capabilities": "runtime"}]):
    one(f"modJson.bad.{i}", P(modJson=v))
for i, v in enumerate([{}, {"enabled": True, "targets": ["server"]}, None]):
    one(f"extendedModJson.ok.{i}", P(extendedModJson=v))
for i, v in enumerate([[], "x", True]):
    one(f"extendedModJson.bad.{i}", P(extendedModJson=v))
for i, v in enumerate([{}, {"locales/de.json": "{}"}, {"src/a.lua": ""}, {f"f{n}.txt": "" for n in range(64)}]):
    one(f"packageFiles.ok.{i}", P(packageFiles=v))
for i, v in enumerate([{"mod.json": "{}"}, {"extended.mod.json": "{}"}, {"../x": ""}, {"a\\b": ""}, {"": ""}, {"/abs": ""}, {"c:x": ""},
                       {f"f{n}.txt": "" for n in range(65)}, {"a": 1}, [], {"./a": ""}]):
    one(f"packageFiles.bad.{i}", P(packageFiles=v))
TPL = lambda files: d(P(packageFiles=files)).replace('"@@X"', '"@@"')
case("packageFiles.size.ok-1mib", {"a.json": {"template": TPL({"big.txt": "@@X"}), "fill": "x", "count": 1024 * 1024}})
case("packageFiles.size.bad-1mib", {"a.json": {"template": TPL({"big.txt": "@@X"}), "fill": "x", "count": 1024 * 1024 + 1}})
case("packageFiles.size.ok-4mib", {"a.json": {"template": d(P(packageFiles={f"f{n}": "@@X" for n in range(4)})).replace('"@@X"', '"@@"'), "fill": "x", "count": 1024 * 1024}})
case("packageFiles.size.bad-4mib", {"a.json": {"template": d(P(packageFiles={f"f{n}": "@@X" for n in range(5)})).replace('"@@X"', '"@@"'), "fill": "x", "count": 900 * 1024}})
case("packageFiles.size.multibyte", {"a.json": {"template": TPL({"big.txt": "@@X"}), "fill": "é", "count": 512 * 1024 + 1}})
for i, v in enumerate([{}, {"a.dll": "bin/a.dll"}, {f"s{n}": f"d{n}" for n in range(64)}]):
    one(f"importFiles.ok.{i}", P(importFiles=v))
for i, v in enumerate([{"../a": "b"}, {"a": "../b"}, {"a": ""}, {"a\\b": "c"}, {f"s{n}": f"d{n}" for n in range(65)}, {"a": 1}, []]):
    one(f"importFiles.bad.{i}", P(importFiles=v))
for i, v in enumerate([[], [{"path": "a.dll"}], [{"path": "bin/a.DLL", "mode": "dinput8"}], [{"path": "a.dll", "mode": "library"}],
                       [{"path": f"f{n}.dll"} for n in range(16)]]):
    one(f"files.ok.{i}", P(files=v))
for i, v in enumerate([[{"path": "a.txt"}], [{"path": "a.dll"}, {"path": "A.DLL"}], [{"path": "../a.dll"}], [{"path": "a\\b.dll"}],
                       [{"path": f"f{n}.dll"} for n in range(17)], [{"path": "a.dll", "mode": "other"}], [{"path": "a.dll", "extra": 1}],
                       [{"mode": "library"}], [{"path": 1}], {}, [{"path": ".dll"}], [{"path": "c:a.dll"}], [{"path": "a.dll", "mode": None}]]):
    one(f"files.bad.{i}", P(files=v))
one("unknown-field", P(extra=1))
one("unknown-field-snake", P(schema_version=1))
case("json.invalid", {"a.json": '{"schemaVersion":1,'})
case("json.trailing-comma", {"a.json": d(P())[:-1] + ",}"})
case("json.duplicate-key", {"a.json": '{"schemaVersion":1,"id":"x","id":"p1","packageDirectory":"pkg","name":"P"}'})
case("catalog.duplicate-id", {"b.json": d(P(name="B")), "a.json": d(P(name="A")), "c.json": d(P(id="p2"))})
case("catalog.sorted", {"z.json": d(P(id="z")), "A.json": d(P(id="A")), "m.json": d(P(id="m"))})
case("catalog.extension", {"a.JSON": d(P(id="upper")), "b.txt": d(P(id="txt")), "c.json.bak": d(P(id="bak")), "d.Json": d(P(id="mixed"))})
case("catalog.invalid-skipped", {"a.json": "nope", "b.json": d(P(id="good"))})
# overlays on a package
def ov(cid, profile, package=PKG, **kw):
    case(cid, {"a.json": d(profile)}, package=package, **kw)
ov("overlay.none", P(packageDirectory="other"))
ov("overlay.directory-case", P(packageDirectory="PKG", modJson={"description": "from profile"}))
ov("overlay.modid-match", P(modId="example", packageDirectory="other", modJson={"description": "by id"}))
ov("overlay.modid-mismatch", P(modId="different", packageDirectory="pkg", modJson={"description": "x"}))
ov("overlay.capabilities-union", P(modJson={"capabilities": ["patch", "runtime"]}))
ov("overlay.capabilities-noncanonical", P(modJson={"capabilities": ["runtime-register-dll"]}), package={"mod.json": d(dict(MOD, capabilities="bad")), "extended.mod.json": d(EXT)})
ov("overlay.capabilities-missing", P(modJson={"capabilities": ["export"]}), package={"mod.json": d({k: v for k, v in MOD.items() if k != "capabilities"}), "extended.mod.json": d(EXT)})
ov("overlay.recursive", P(modJson={"authors": ["x"], "description": "d"}, extendedModJson={"settings": {"level": {"label": "Overlay"}, "flag": {"label": "L"}, "added": 3}}))
ov("overlay.enabled-stripped", P(extendedModJson={"enabled": False, "targets": ["server"]}))
ov("overlay.synthesized-extension", P(extendedModJson={"targets": ["client"]}), package={"mod.json": d(MOD)})
ov("overlay.synthesized-empty", P(extendedModJson={}), package={"mod.json": d(MOD)})
ov("overlay.no-extension-overlay", P(modJson={"description": "d"}), package={"mod.json": d(MOD)})
ov("overlay.package-files", P(packageFiles={"locales/de.json": d({"mod": {"name": "Deutsch"}})}))
ov("overlay.package-files-invalid-locale", P(packageFiles={"locales/de.json": "{"}))
ov("overlay.package-files-override", P(packageFiles={"locales/de.json": d({"mod": {"name": "Virtual"}})}),
   package=dict(PKG, **{"locales/de.json": d({"mod": {"name": "Disk"}})}))
ov("overlay.makes-invalid", P(modJson={"name": ""}))
ov("overlay.extension-makes-invalid", P(extendedModJson={"schemaVersion": 2}))
ov("overlay.null-modjson", P(modJson=None))
SEL = lambda prof, directory="pkg": [{"id": "sel", "packageDirectory": directory, "name": "Sel", "target": "both", "enabled": False, "approved": False, "files": [], "compatibilityProfile": prof}]
case("select.explicit", {"a.json": d(P(id="auto", modJson={"description": "auto"})), "b.json": d(P(id="chosen", packageDirectory="elsewhere", modId="nobody", modJson={"description": "chosen"}))},
     package=PKG, selections=SEL("chosen"))
case("select.explicit-missing", {"a.json": d(P(id="auto", modJson={"description": "auto"}))}, package=PKG, selections=SEL("absent"))
case("select.none", {"a.json": d(P(id="auto", modJson={"description": "auto"}))}, package=PKG, selections=SEL("none"))
case("select.empty-string", {"a.json": d(P(id="auto", modJson={"description": "auto"}))}, package=PKG, selections=SEL(""))
case("select.null", {"a.json": d(P(id="auto", modJson={"description": "auto"}))}, package=PKG, selections=SEL(None))
case("select.other-directory", {"a.json": d(P(id="auto", modJson={"description": "auto"})), "b.json": d(P(id="chosen", modJson={"description": "chosen"}))},
     package=PKG, selections=SEL("chosen", "other"))
case("select.directory-case", {"b.json": d(P(id="chosen", packageDirectory="zzz", modJson={"description": "chosen"}))}, package=PKG, selections=SEL("chosen", "PKG"))
case("select.first-auto", {"b.json": d(P(id="second", modJson={"description": "second"})), "a.json": d(P(id="first", modJson={"description": "first"}))}, package=PKG)
case("select.invalid-skipped-auto", {"a.json": d(P(id="bad", name="")), "b.json": d(P(id="good", modJson={"description": "good"}))}, package=PKG)
json.dump(cases, open(sys.argv[1], "w"), ensure_ascii=False, indent=0)
print(len(cases))
