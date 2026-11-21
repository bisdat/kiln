# SPDX-License-Identifier: MIT
# Generates the package-document differential cases.
# Each case is a package directory (files) read by ShroudForge's read_manifest_path
# at both pins (scratch harness) and by Kiln's sfi_package::discover::read_package.
import json, sys

cases = []
def dump(v):
    return json.dumps(v, separators=(",", ":"), ensure_ascii=False)
BASE_M = {"id": "example", "name": "Example", "version": "1.0.0"}
BASE_E = {"schemaVersion": 1, "enabled": True}
DEL = object()
def patch(base, upd):
    out = dict(base)
    for k, v in (upd or {}).items():
        if v is DEL:
            out.pop(k, None)
        else:
            out[k] = v
    return out
def case(cid, m=None, e=None, *, mraw=None, eraw=None, noext=False, strict=False,
         locales=None, save=None, resolve=None, strings=None, hexfiles=None, extra=None, enabled_values=None):
    files = {}
    files["mod.json"] = mraw if mraw is not None else dump(patch(BASE_M, m))
    if not noext:
        files["extended.mod.json"] = eraw if eraw is not None else dump(patch(BASE_E, e))
    for name, v in (locales or {}).items():
        files["locales/" + name] = v if isinstance(v, str) else dump(v)
    for name, v in (extra or {}).items():
        files[name] = v
    c = {"id": cid, "files": files, "strict": strict}
    if enabled_values is not None: c["enabled_values"] = enabled_values
    if save is not None: c["save"] = save
    if resolve is not None: c["resolve"] = resolve
    if strings is not None: c["strings"] = strings
    if hexfiles: c["files_hex"] = hexfiles
    assert cid not in {x["id"] for x in cases}, cid
    cases.append(c)

S = lambda **kw: {"settings": kw}

# ---- mod.json document ----
case("mod.ok-minimal")
case("mod.ok-no-extension", noext=True)
for i, raw in enumerate(["[]", "null", "false", "\"text\"", "1"]):
    case(f"mod.nonobject.{i}", mraw=raw)
for k in ["id", "name", "version"]:
    case(f"mod.missing.{k}", {k: DEL})
case("mod.missing.all", mraw="{}")
case("mod.unknown.tolerant", {"foreign": {"keep": True}, "settings": {"x": 1}, "enabled": True})
case("mod.unknown.strict-one", {"foreign": True}, strict=True)
case("mod.unknown.strict-many", mraw='{"id":"example","name":"Example","version":"1.0.0","z":1,"a":2,"z":3}', strict=True)
case("mod.unknown.strict-before-schema", {"name": False, "foreign": True}, strict=True)
case("mod.both-invalid.order", {"name": False}, {"enabled": None})
case("mod.both-invalid.strict-order", {"foreign": True}, {"foreign": True}, strict=True)
case("mod.both-nonobject", mraw="[]", eraw="[]")
case("mod.duplicate-keys", mraw='{"id":"first","name":"Example","version":"1.0.0","id":"example"}')
case("mod.json.trailing-comma", mraw='{"id":"example","name":"Example","version":"1.0.0",}')
case("mod.json.comment", mraw='{/*c*/"id":"example","name":"Example","version":"1.0.0"}')
case("mod.json.lone-surrogate", mraw='{"id":"example","name":"\\uD800","version":"1.0.0"}')
case("mod.json.overflow", mraw='{"id":"example","name":"Example","version":"1.0.0","description":1e999}')
case("mod.json.empty", mraw="")
case("mod.json.bom", mraw="", hexfiles={"mod.json": "efbbbf" + dump(BASE_M).encode().hex()})
case("mod.json.invalid-utf8", mraw="", hexfiles={"mod.json": dump(patch(BASE_M, {"name": "X"})).encode().hex().replace("58", "ff", 1)})
case("ext.json.trailing-comma", eraw='{"schemaVersion":1,"enabled":true,}')
# ---- mod.json keys ----
for i, v in enumerate(["a", "A.b_c-d", "x" * 80, "Example.ID", "-", "_"]):
    case(f"mod.id.ok.{i}", {"id": v})
for i, v in enumerate(["", "bad id", "x" * 81, ".", "..", "é", 1, None, ["a"], "a/b"]):
    case(f"mod.id.bad.{i}", {"id": v})
for i, v in enumerate(["N", "n" * 120, "é" * 120, " "]):
    case(f"mod.name.ok.{i}", {"name": v})
for i, v in enumerate(["", "n" * 121, "é" * 121, 1, None, True]):
    case(f"mod.name.bad.{i}", {"name": v})
for i, v in enumerate(["0.0.0", "1.2.3-alpha.1", "1.2.3+build.5", "1.2.3-rc-1", "18446744073709551615.0.0"]):
    case(f"mod.version.ok.{i}", {"version": v})
for i, v in enumerate(["1.2", "v1.2.3", "01.2.3", "1.02.3", "1.2.3-a+b", "1.2.3-", "1.2.3-01", "18446744073709551616.0.0", "1.2.3 ", 1, None, ""]):
    case(f"mod.version.bad.{i}", {"version": v})
for i, v in enumerate(["", "d" * 2000, "é" * 2000]):
    case(f"mod.description.ok.{i}", {"description": v})
for i, v in enumerate(["d" * 2001, None, 1, ["x"]]):
    case(f"mod.description.bad.{i}", {"description": v})
for i, v in enumerate([[], ["b", "a"], ["A", "a"], ["é"]]):
    case(f"mod.authors.ok.{i}", {"authors": v})
for i, v in enumerate([["a", "a"], [""], [1], "a", None, [None], ["a", ""]]):
    case(f"mod.authors.bad.{i}", {"authors": v})
for i, v in enumerate(["MIT", None, "", "not spdx (c)"]):
    case(f"mod.license.ok.{i}", {"license": v})
for i, v in enumerate([False, 1, ["MIT"], {}]):
    case(f"mod.license.bad.{i}", {"license": v})
for i, v in enumerate([None, "icon.png", "assets/x/y.png", "_x", "a.b.c", "assets/../x.png", "assets/"]):
    case(f"mod.icon.ok.{i}", {"icon": v})
for i, v in enumerate(["../bad.png", ".hidden", "a b.png", "dir/icon.png", "", 1, "assets", "assets\\x.png"]):
    case(f"mod.icon.bad.{i}", {"icon": v})
case("mod.$schema.ok", {"$schema": "https://example.org/anything.json"})
case("mod.$schema.bad", {"$schema": 1})
case("mod.$schema.null", {"$schema": None})
for cap in ["patch", "export", "runtime", "runtime-register-dll"]:
    case(f"mod.capabilities.ok.{cap}", {"capabilities": [cap]})
case("mod.capabilities.ok.all", {"capabilities": ["runtime-register-dll", "runtime", "export", "patch"]})
case("mod.capabilities.ok.empty", {"capabilities": []})
for i, v in enumerate([["native"], ["runtime", "runtime"], ["Patch"], [1], "runtime", None, [None]]):
    case(f"mod.capabilities.bad.{i}", {"capabilities": v})
dep = lambda **kw: patch({"id": "dep", "version": "^1.0.0"}, kw)
for i, v in enumerate([[], [dep()], [dep(id="b"), dep(id="a")], [dep(version=">=1.2.0, <2.0.0")], [dep(version="*")],
                       [dep(version="=1.0.0")], [dep(version="~1.2")], [dep(optional=True)], [dep(optional=False)],
                       [dep(id="shroudforge-api", version="^1.0")], [dep(id="Dep")], [dep(version="1")], [dep(), dep()]]):
    case(f"mod.dependencies.ok.{i}", {"dependencies": v})
for i, v in enumerate([{}, "dep", None, [None], ["dep"], [{"id": "dep"}], [{"version": "*"}], [dep(extra=1)],
                       [dep(id="bad id")], [dep(id="")], [dep(id="x" * 81)], [dep(id=1)], [dep(version="not a range")],
                       [dep(version="")], [dep(version=1)], [dep(optional="yes")], [dep(optional=None)],
                       [dep(version=">=1.0 <2.0")], [dep(id=".")]]):
    case(f"mod.dependencies.bad.{i}", {"dependencies": v})
case("mod.dependencies.bad-order", {"dependencies": [{"version": "*", "id": "bad id", "z": 1}]})

# ---- extended.mod.json document ----
for i, raw in enumerate(["[]", "null", "true", "\"x\"", "1"]):
    case(f"ext.nonobject.{i}", eraw=raw)
case("ext.missing.schemaVersion", None, {"schemaVersion": DEL})
case("ext.missing.enabled", None, {"enabled": DEL})
case("ext.missing.both", eraw="{}")
case("ext.unknown.tolerant", None, {"foreign": 1})
case("ext.unknown.strict-one", None, {"foreign": 1}, strict=True)
case("ext.unknown.strict-many", None, {"z": 1, "a": 2}, strict=True)
case("ext.18-blocks.tolerant", None, {"inputReservations": {"bogus": True}, "presets": {"bogus": True}})
case("ext.18-blocks.strict", None, {"inputReservations": {}}, strict=True)
case("ext.$schema.ok", None, {"$schema": "https://example.org/x.json"})
case("ext.$schema.bad", None, {"$schema": False})
for i, v in enumerate([1, 1.0, 1e0]):
    case(f"ext.schemaVersion.ok.{i}", None, {"schemaVersion": v})
for i, v in enumerate([2, 0, "1", True, None, 1.5, [1]]):
    case(f"ext.schemaVersion.bad.{i}", None, {"schemaVersion": v})
for i, v in enumerate([True, False]):
    case(f"ext.enabled.ok.{i}", None, {"enabled": v})
for i, v in enumerate([None, "true", 1, 0]):
    case(f"ext.enabled.bad.{i}", None, {"enabled": v})
for i, v in enumerate([["client"], ["server"], ["client", "server"], ["server", "client"]]):
    case(f"ext.targets.ok.{i}", None, {"targets": v})
    case(f"ext.targets.ok-eml.{i}", None, {"targets": v, "launcher": "EML"})
for i, v in enumerate([[], ["x"], ["client", "client"], "client", [1], None, ["client", "server", "client"], ["Client"]]):
    case(f"ext.targets.bad.{i}", None, {"targets": v})
for i, v in enumerate(["EML", "SF"]):
    case(f"ext.launcher.ok.{i}", None, {"launcher": v})
for i, v in enumerate(["eml", "Other", 1, None]):
    case(f"ext.launcher.bad.{i}", None, {"launcher": v})
LINK_IDS = ["source", "source-github", "source-gitlab", "source-codeberg", "issues", "wiki", "website", "store",
            "support-bmac", "support-patreon", "support-paypal", "support-github", "support-ko-fi",
            "support-open-collective", "support-other"]
for lid in LINK_IDS:
    case(f"ext.links.ok.{lid}", None, {"links": {lid: "https://example.org/" + lid}})
    case(f"ext.links.http.{lid}", None, {"links": {lid: "http://example.org/" + lid}})
case("ext.links.ok.all", None, {"links": {lid: "https://e.org" for lid in reversed(LINK_IDS)}})
case("ext.links.ok.empty", None, {"links": {}})
case("ext.links.ok.max", None, {"links": {"website": "https://" + "a" * 2040}})
case("ext.links.ok.bare-scheme", None, {"links": {"website": "https://"}})
for i, v in enumerate([{"made-up": "https://e.org"}, {"Bad": "https://e.org"}, {"1x": "https://e.org"}, {"a_b": "https://e.org"},
                       {"website": "https://" + "a" * 2041}, {"website": 1}, {"website": None}, {"website": "HTTPS://e.org"},
                       {"website": " https://e.org"}, [], "https://e.org", None, {"": "https://e.org"},
                       {"made-up": "http://e.org"}, {"website": "ftp://e.org", "made-up": "https://e.org"}]):
    case(f"ext.links.bad.{i}", None, {"links": v})
for i, v in enumerate([[], ["one"], ["c"] * 50, ["c" * 500], ["é" * 500], ["b", "a", "b"]]):
    case(f"ext.changelog.ok.{i}", None, {"changelog": v})
for i, v in enumerate([["c"] * 51, [""], ["c" * 501], [1], "c", None, [None]]):
    case(f"ext.changelog.bad.{i}", None, {"changelog": v})

# ---- unknown top-level keys: removal order (pkg-unknown-order) ----
# SF drops each unknown top-level key in turn with serde_json preserve_order's Map::remove,
# which is swap_remove (serde_json 1.0.151 map.rs:156-165): the last key moves into the gap.
# The closed schemas report a known field's error in instance order, so with several bad
# known fields the order the removals leave decides which error comes first. Each layout
# runs tolerant (the reordered document) and strict (the unknown-field list, document order).
UNK_M = [
    ("none", '{"name":false,"id":1,"version":"1.0.0"}'),
    ("one-first", '{"foreign":1,"name":false,"version":"1.0.0","id":1}'),
    ("one-middle", '{"version":"1.0.0","foreign":1,"name":false,"description":5}'),
    ("one-last", '{"name":false,"id":1,"version":"1.0.0","foreign":1}'),
    ("two-apart", '{"a":1,"b":2,"name":false,"version":"1.0.0","id":1,"description":5}'),
    ("two-adjacent", '{"name":false,"a":1,"b":2,"version":"1.0.0","id":1,"description":5}'),
    ("two-ends", '{"a":1,"name":false,"id":1,"version":"bad","z":2}'),
    ("two-last", '{"name":false,"id":1,"version":"bad","y":1,"z":2}'),
    ("three-interleaved", '{"x":1,"name":false,"y":2,"id":1,"z":3,"version":"bad","description":5}'),
    ("four-interleaved", '{"name":false,"w":0,"id":1,"x":1,"version":"bad","y":2,"description":5,"z":3,"license":7}'),
    ("five-leading", '{"p":1,"q":2,"r":3,"s":4,"t":5,"name":false,"id":1,"version":"bad","icon":1}'),
    ("dup-unknown", '{"foo":1,"name":false,"foo":2,"id":1,"version":"bad"}'),
    ("dup-known", '{"foreign":1,"name":false,"id":1,"version":"1.0.0","name":"Example"}'),
    ("case-variants", '{"ID":1,"Name":2,"name":false,"version":"1.0.0","id":1}'),
    ("schema-like", '{"$Schema":1,"name":false,"$schema":2,"id":1,"version":"1.0.0"}'),
    ("valid", '{"a":1,"id":"example","b":{"c":2},"name":"Example","version":"1.0.0","z":[3]}'),
]
for name, raw in UNK_M:
    case(f"mod.unknown-order.{name}", mraw=raw)
    case(f"mod.unknown-order.{name}.strict", mraw=raw, strict=True)
UNK_E = [
    ("none", '{"enabled":null,"schemaVersion":1,"targets":[]}'),
    ("one-first", '{"foreign":1,"enabled":null,"schemaVersion":1,"targets":[]}'),
    ("two-apart", '{"a":1,"enabled":null,"b":2,"schemaVersion":2,"targets":[]}'),
    ("three-interleaved", '{"x":1,"launcher":"x","y":2,"enabled":null,"z":3,"schemaVersion":1,"changelog":[1]}'),
    ("dup-unknown", '{"foo":1,"enabled":null,"foo":2,"schemaVersion":1,"links":[]}'),
    ("valid", '{"a":1,"schemaVersion":1,"b":2,"enabled":true,"c":3,"targets":["server"]}'),
]
for name, raw in UNK_E:
    case(f"ext.unknown-order.{name}", eraw=raw)
    case(f"ext.unknown-order.{name}.strict", eraw=raw, strict=True)
case("both.unknown-order", mraw=UNK_M[1][1], eraw=UNK_E[1][1])

# ---- settings ----
case("set.ok.empty", None, S())
case("set.ok.shorthand", None, S(flag=True, text="t", count=2, ratio=0.5, big=1e2, neg=-0, items=["a"]),)
case("set.bad.shorthand-array-no-options", None, S(items=["a"]))
case("set.ok.order-dup", None, eraw='{"schemaVersion":1,"enabled":true,"settings":{"z":1,"a":true,"z":2}}')
for i, v in enumerate([None, {}, {"label": "x"}]):
    case(f"set.bad.entry.{i}", None, S(a=v))
case("set.ok.key-80", None, {"settings": {"k" * 80: True}})
for i, k in enumerate(["k" * 81, "", "bad key", "é", "a/b"]):
    case(f"set.bad.key.{i}", None, {"settings": {k: True}})
case("set.bad.nonobject", None, {"settings": []})
case("set.bad.null", None, {"settings": None})
# value
for i, v in enumerate([True, "s", 1, 1.5, -0.0, 9223372036854775807, 18446744073709551615, 1e2, 2.0]):
    case(f"set.value.ok.{i}", None, S(a={"value": v}))
for i, v in enumerate([None, {"x": 1}]):
    case(f"set.value.bad.{i}", None, S(a={"value": v}))
case("set.value.array-options", None, S(a={"value": [1, 2], "options": {"1": "One", "2": "Two"}}))
case("set.value.array-not-option", None, S(a={"value": [3], "options": {"1": "One"}}))
case("set.value.array-empty", None, S(a={"value": [], "options": {"1": "One"}}))
case("set.value.array-dup", None, S(a={"value": [1, 1], "options": {"1": "One"}}))
case("set.value.scalar-not-option", None, S(a={"value": "c", "options": {"a": "A", "b": "B"}}))
# label / description
for i, (k, v) in enumerate([("myKeyName", {}), ("URLValue", {}), ("aÄb", {}), ("x", {"label": "L"}), ("x", {"label": "l" * 120})]):
    case(f"set.label.ok.{i}", None, {"settings": {k: patch({"value": True}, v)}})
for i, v in enumerate(["", "l" * 121, 1, None]):
    case(f"set.label.bad.{i}", None, S(a={"value": True, "label": v}))
for i, v in enumerate(["", "d" * 500]):
    case(f"set.description.ok.{i}", None, S(a={"value": True, "description": v}))
for i, v in enumerate(["d" * 501, 1, None]):
    case(f"set.description.bad.{i}", None, S(a={"value": True, "description": v}))
# control
CONTROLS = ["toggle", "checkbox", "text", "textarea", "number", "slider", "select", "radio", "segmented", "multiselect", "keybind", "color"]
for c in CONTROLS:
    for j, val in enumerate([True, "s", 1, 0.5, ["a"], ""]):
        d = {"value": val, "control": c}
        if isinstance(val, list):
            d["options"] = {"a": "A"}
        case(f"set.control.{c}.{j}", None, S(a=d))
    case(f"set.control.{c}.with-options", None, S(a={"value": "a", "control": c, "options": {"a": "A", "b": "B"}}))
for i, v in enumerate(["button", "Toggle", 1, None]):
    case(f"set.control.bad.{i}", None, S(a={"value": True, "control": v}))
case("set.control.infer-select", None, S(a={"value": "a", "options": {"a": "A"}}))
case("set.control.infer-multiselect", None, S(a={"value": ["a"], "options": {"a": "A"}}))
# applyAt / restartRequired / assetPreparation
for i, d in enumerate([{"applyAt": "live"}, {"applyAt": "restart"}, {"restartRequired": True}, {"restartRequired": False},
                       {"applyAt": "live", "restartRequired": False}, {"applyAt": "restart", "restartRequired": True},
                       {"assetPreparation": True}, {"assetPreparation": False}]):
    case(f"set.schedule.ok.{i}", None, S(a=patch({"value": True}, d)))
for i, d in enumerate([{"applyAt": "later"}, {"applyAt": None}, {"applyAt": "live", "restartRequired": True},
                       {"applyAt": "restart", "restartRequired": False}, {"restartRequired": None}, {"restartRequired": "yes"},
                       {"assetPreparation": 1}, {"assetPreparation": None}]):
    case(f"set.schedule.bad.{i}", None, S(a=patch({"value": True}, d)))
# enabledWhen
EW = lambda s, eq: {"setting": s, "equals": eq}
for i, (ctl, eq) in enumerate([(True, False), ("x", "x"), (1, 1), (1.5, 2), (1.5, 2.5), ({"value": "a", "options": {"a": "A", "b": "B"}}, "b"),
                               ({"value": 1, "options": {"1": "One"}}, 1)]):
    case(f"set.enabledWhen.ok.{i}", None, {"settings": {"c": ctl, "a": {"value": True, "enabledWhen": EW("c", eq)}}})
for i, (ctl, eq) in enumerate([(True, "true"), ("x", 1), (1, 1.5), (1, True), (["a"], "a"), ({"value": "a", "options": {"a": "A"}}, "z"),
                               ({"value": 1, "options": {"1": "One"}}, 1.0), (1.5, "x")]):
    d = ctl if not isinstance(ctl, list) else {"value": ctl, "options": {"a": "A"}}
    case(f"set.enabledWhen.bad.{i}", None, {"settings": {"c": d, "a": {"value": True, "enabledWhen": EW("c", eq)}}})
case("set.enabledWhen.self", None, S(a={"value": True, "enabledWhen": EW("a", True)}))
case("set.enabledWhen.unknown", None, S(a={"value": True, "enabledWhen": EW("missing", True)}))
case("set.enabledWhen.cycle", None, {"settings": {"a": {"value": True, "enabledWhen": EW("b", True)}, "b": {"value": True, "enabledWhen": EW("a", True)}}})
case("set.enabledWhen.chain", None, {"settings": {"a": {"value": True, "enabledWhen": EW("b", True)}, "b": {"value": True, "enabledWhen": EW("c", True)}, "c": True}})
for i, d in enumerate([{"setting": "c"}, {"equals": True}, {"setting": "c", "equals": True, "x": 1}, {"setting": "bad key", "equals": True},
                       {"setting": "c", "equals": None}, {"setting": "c", "equals": [True]}, {"setting": "c", "equals": {}}, "c", {"setting": 1, "equals": True},
                       {"setting": "k" * 81, "equals": True}]):
    case(f"set.enabledWhen.schema.{i}", None, {"settings": {"c": True, "a": {"value": True, "enabledWhen": d}}})
# numeric bounds
for i, d in enumerate([{"min": 0, "max": 10}, {"min": 1, "max": 1}, {"min": -1.5}, {"max": 1e3}, {"min": 0, "value": 0}, {"step": 0.25}, {"step": 1e-9}, {"step": 3}]):
    case(f"set.numeric.ok.{i}", None, S(a=patch({"value": 1}, d)))
for i, d in enumerate([{"min": 2}, {"max": 0}, {"min": 3, "max": 2}, {"step": 0}, {"step": -1}, {"min": "0"}, {"max": None}, {"step": "1"}, {"step": None}]):
    case(f"set.numeric.bad.{i}", None, S(a=patch({"value": 1}, d)))
case("set.numeric.off-step", None, S(a={"value": 1.3, "step": 1, "min": 0}))
case("set.numeric.bool-min", None, S(a={"value": True, "min": 5}))
case("set.numeric.string-max", None, S(a={"value": "abc", "max": 0}))
# length
for i, d in enumerate([{"minLength": 0}, {"minLength": 3, "value": "abc"}, {"maxLength": 3, "value": "abc"}, {"minLength": 4.0, "value": "ab"},
                       {"maxLength": 1.0, "value": "abc"}, {"minLength": 5, "maxLength": 1, "value": "abc"}, {"maxLength": 1, "value": "é"}]):
    case(f"set.length.ok.{i}", None, S(a=patch({"value": "ab"}, d)))
for i, d in enumerate([{"minLength": 3}, {"maxLength": 1}, {"minLength": -1}, {"maxLength": 1.5}, {"minLength": "1"}, {"maxLength": None}, {"minLength": 5, "maxLength": 1}]):
    case(f"set.length.bad.{i}", None, S(a=patch({"value": "ab"}, d)))
# options
for i, d in enumerate([{"value": True, "options": {"true": "Yes", "false": "No"}}, {"value": True, "options": {"true": "Yes", "anything": "No"}},
                       {"value": 1, "options": {"1": "One", "01": "Also one"}}, {"value": 1.5, "options": {"1.5": "x", "NaN": "n"}},
                       {"value": "a", "options": {"a": "A", "1": "One"}}, {"value": "a", "options": {"a": "l" * 120}},
                       {"value": [True], "options": {"true": "T"}}, {"value": 1.0, "options": {"1": "One"}}]):
    case(f"set.options.ok.{i}", None, S(a=d))
for i, d in enumerate([{"value": "a", "options": {}}, {"value": "a", "options": {"a": ""}}, {"value": "a", "options": {"a": "l" * 121}},
                       {"value": "a", "options": {"a": 1}}, {"value": "a", "options": []}, {"value": "a", "options": None},
                       {"value": 1, "options": {"x": "X"}}, {"value": "a", "options": {"a": {"label": "A"}}},
                       {"value": "a", "options": {"a": {"label": "A", "group": "G"}}}, {"value": "a", "options": {"a": {"label": "A", "extra": 1}}},
                       {"value": "a", "options": {"a": {"label": ""}}}, {"value": "a", "options": {"a": {"group": "G"}}},
                       {"value": "a", "options": {"a": {"label": "A", "group": "g" * 61}}}, {"value": "a", "options": {"a": {"label": "A", "group": ""}}}]):
    case(f"set.options.case.{i}", None, S(a=d))
# keybind
for i, v in enumerate(["", "SDL:4", "Ctrl+Alt+Shift+Meta+SDL:4", "Mouse:1", "Mouse:32", "SDL:4|Mouse:3", "Ctrl+SDL:4|Shift+Mouse:3",
                       "SDL:999", "SDL:0", "Mouse:33", "Mouse:0", "Alt+Ctrl+SDL:4", "SDL:04", "sdl:4", "SDL:4|SDL:5", "Mouse:3|SDL:4",
                       "Ctrl+" * 20 + "SDL:4", "SDL:224", "Ctrl+SDL:224", "SDL:511", "SDL:512", 4, None, "Ctrl+Ctrl+SDL:4", "Hyper+SDL:4"]):
    case(f"set.keybind.{i}", None, S(a={"value": v, "control": "keybind"}))
# save-path validation (validate_settings) for package.settings.validation
SAVE_DEFS = {"flag": True, "count": {"value": 2, "min": 0, "max": 5}, "ratio": {"value": 0.5, "min": 0, "max": 1, "step": 0.25},
             "name": {"value": "ab", "minLength": 1, "maxLength": 3}, "mode": {"value": "a", "options": {"a": "A", "b": "B"}},
             "many": {"value": [1], "options": {"1": "One", "2": "Two", "3": "Three", "4": "Four"}},
             "key": {"value": "SDL:4", "control": "keybind"}, "free": "x", "wide": {"value": 1, "options": {"1": "One", "2": "Two", "3": "Three"}}}
for i, sv in enumerate([{}, {"flag": False}, {"count": 5}, {"count": 0}, {"ratio": 0.3}, {"name": "abc"}, {"mode": "b"}, {"many": [1, 2, 2]}, {"many": []},
                        {"key": ""}, {"key": "Mouse:3"}, {"free": "y"},
                        {"flag": 1}, {"count": 6}, {"count": -1}, {"count": 1.5}, {"count": 2.0}, {"ratio": 2}, {"name": ""}, {"name": "abcd"}, {"mode": "c"},
                        {"many": [5]}, {"many": 1}, {"key": "SDL:0"}, {"key": "Alt+Ctrl+SDL:4"}, {"key": 4}, {"free": 1}, {"unknown": 1},
                        {"wide": 4}, {"mode": None}, {"count": "1"}, {"flag": False, "count": 9, "zzz": 1}, [], None, "x"]):
    case(f"save.{i}", None, {"settings": SAVE_DEFS}, save=sv)

# ---- groups / actions ----
GS = {"settings": {"a": True, "b": True}}
def G(*groups): return patch(GS, {"groups": list(groups)})
for i, g in enumerate([{"label": "G"}, {"label": "G", "description": ""}, {"label": "G", "description": "d" * 500}, {"label": "l" * 120},
                       {"label": "G", "settings": []}, {"label": "G", "settings": ["b", "a"]}, {"label": "G", "actions": []},
                       {"label": "G", "actions": [{"id": "x", "label": "X"}, {"id": "x", "label": "Y"}]},
                       {"label": "G", "actions": [{"id": "x", "label": "X", "style": s} for s in ["primary", "secondary", "danger"]]},
                       {"label": "G", "actions": [{"id": "k" * 80, "label": "l" * 120, "confirm": "c" * 500}]},
                       {"label": "G", "actions": [{"id": "x", "label": "X", "confirm": ""}]}, {"label": "G", "tab": True}, {"label": "G", "tab": False}]):
    case(f"grp.ok.{i}", None, G(g))
case("grp.ok.two", None, G({"label": "A", "settings": ["a"]}, {"label": "B", "settings": ["b"]}))
case("grp.ok.empty", None, G())
for i, g in enumerate([{}, {"label": ""}, {"label": " "}, {"label": "\t\n"}, {"label": "l" * 121}, {"label": 1}, {"label": "G", "description": "d" * 501},
                       {"label": "G", "description": None}, {"label": "G", "settings": ["a", "a"]}, {"label": "G", "settings": ["missing"]},
                       {"label": "G", "settings": ["bad key"]}, {"label": "G", "settings": "a"}, {"label": "G", "settings": [1]},
                       {"label": "G", "actions": {}}, {"label": "G", "actions": [{}]}, {"label": "G", "actions": [{"id": "x"}]},
                       {"label": "G", "actions": [{"label": "X"}]}, {"label": "G", "actions": [{"id": "bad id", "label": "X"}]},
                       {"label": "G", "actions": [{"id": "", "label": "X"}]}, {"label": "G", "actions": [{"id": "k" * 81, "label": "X"}]},
                       {"label": "G", "actions": [{"id": "x", "label": ""}]}, {"label": "G", "actions": [{"id": "x", "label": "  "}]},
                       {"label": "G", "actions": [{"id": "x", "label": "l" * 121}]}, {"label": "G", "actions": [{"id": "x", "label": "X", "style": "other"}]},
                       {"label": "G", "actions": [{"id": "x", "label": "X", "style": None}]}, {"label": "G", "actions": [{"id": "x", "label": "X", "confirm": "c" * 501}]},
                       {"label": "G", "actions": [{"id": "x", "label": "X", "confirm": 1}]}, {"label": "G", "actions": [{"id": "x", "label": "X", "extra": 1}]},
                       {"label": "G", "actions": [None]}, {"label": "G", "tab": "yes"}, {"label": "G", "extra": 1}, "G", None]):
    case(f"grp.bad.{i}", None, G(g))
case("grp.bad.twice", None, G({"label": "A", "settings": ["a"]}, {"label": "B", "settings": ["a"]}))
case("grp.bad.nonarray", None, patch(GS, {"groups": {}}))
case("grp.bad.order", None, eraw='{"schemaVersion":1,"enabled":true,"settings":{"a":true},"groups":[{"settings":["missing"],"label":" "}]}')

# ---- locales ----
LS = {"settings": {"flag": {"value": True, "label": "Flag", "description": "D"}, "mode": {"value": "a", "options": {"a": "A", "b": "B"}},
                   "num": {"value": 1, "options": {"1": "One", "2": "Two"}}},
      "groups": [{"label": "G1", "description": "GD", "settings": ["flag"], "actions": [{"id": "go", "label": "Go", "confirm": "Sure?"}]},
                 {"label": "G2", "actions": [{"id": "stop", "label": "Stop"}]}],
      "changelog": ["one", "two", "three"]}
def L(cid, cats, **kw):
    case(cid, None, LS, locales=cats, **kw)
L("loc.ok.none", {})
L("loc.ok.empty", {"de.json": {}})
L("loc.ok.full", {"de.json": {"$schema": "x", "mod": {"name": "N", "description": "D"},
                               "settings": {"flag": {"label": "L", "description": "D"}, "mode": {"options": {"b": "Bee"}}, "num": {"options": {"2": "Zwei"}}},
                               "groups": [{"label": "Gruppe", "description": "B", "actions": {"go": {"label": "Los", "confirm": "Echt?"}}}, {"actions": {"stop": {"label": "Halt"}}}],
                               "changelog": ["eins"], "strings": {"hello": "Hallo", "free.key": "x"}}})
for i, name in enumerate(["en.json", "en-gb.json", "zh-hant-tw.json", "a" * 35 + ".json", "de-1.json", "x1.json"]):
    L(f"loc.name.ok.{i}", {name: {}})
for i, name in enumerate(["e.json", "a" * 36 + ".json", "-de.json", "de-.json", "de--at.json", "de_at.json", "de.at.json"]):
    L(f"loc.name.bad.{i}", {name: {}})
L("loc.name.uppercase-ignored", {"DE.json": "not json", "de.JSON": "not json"})
L("loc.name.nonjson-ignored", {"de.txt": "x", "readme.md": "x"})
L("loc.name.subdir-ignored", {"sub/de.json": "not json"})
for i, cat in enumerate(["[]", "null", "not json", '{"x":1,}', {"foreign": 1}, {"$schema": 1}, {"mod": []}, {"mod": {"name": ""}},
                         {"mod": {"name": "n" * 121}}, {"mod": {"description": "d" * 2001}}, {"mod": {"foreign": 1}}, {"mod": {"name": None}},
                         {"settings": []}, {"settings": {"missing": {}}}, {"settings": {"bad key": {}}}, {"settings": {"flag": {"label": ""}}},
                         {"settings": {"flag": {"label": "l" * 121}}}, {"settings": {"flag": {"description": "d" * 501}}},
                         {"settings": {"flag": {"foreign": 1}}}, {"settings": {"flag": None}}, {"settings": {"mode": {"options": {"z": "Z"}}}},
                         {"settings": {"mode": {"options": {"a": ""}}}}, {"settings": {"mode": {"options": {"a": "l" * 121}}}},
                         {"settings": {"num": {"options": {"01": "x"}}}}, {"settings": {"num": {"options": {"1.0": "x"}}}},
                         {"settings": {"flag": {"options": {"true": "x"}}}}, {"settings": {"mode": {"optionGroups": {"g": "G"}}}},
                         {"groups": {}}, {"groups": [{}] * 51}, {"groups": [{}, {}, {}]}, {"groups": [{"label": ""}]}, {"groups": [{"label": "l" * 121}]},
                         {"groups": [{"description": "d" * 501}]}, {"groups": [{"foreign": 1}]}, {"groups": [{"actions": []}]},
                         {"groups": [{"actions": {"missing": {}}}]}, {"groups": [{"actions": {"stop": {}}}]}, {"groups": [{"actions": {"bad id": {}}}]},
                         {"groups": [{"actions": {"go": {"label": ""}}}]}, {"groups": [{"actions": {"go": {"confirm": "c" * 501}}}]},
                         {"groups": [{"actions": {"go": {"foreign": 1}}}]}, {"groups": [{"actions": {"go": {"label": None}}}]},
                         {"groups": [None]}, {"changelog": ["a", "b", "c", "d"]}, {"changelog": [""]}, {"changelog": ["c" * 501]},
                         {"changelog": ["c"] * 51}, {"changelog": "x"}, {"strings": []}, {"strings": {"bad key": "x"}}, {"strings": {"k": ""}},
                         {"strings": {"k": "s" * 4001}}, {"strings": {"k": 1}}, {"strings": {"k" * 81: "x"}}, {"mod": {"name": "N"}, "z": 1, "a": 2}]):
    L(f"loc.doc.{i}", {"de.json": cat})
for i, cat in enumerate([{"strings": {"k": "s" * 4000}}, {"strings": {"k" * 80: "x"}}, {"changelog": ["a", "b", "c"]}, {"groups": [{}, {}]},
                         {"mod": {"name": "n" * 120, "description": "d" * 2000}}, {"settings": {"flag": {"options": {}}}}]):
    L(f"loc.ok.{i}", {"de.json": cat})
L("loc.dup-case", {"de.json": {}, "DE.json": {}})
L("loc.dup-case-lower", {"de-at.json": {}, "de-AT.json": {}})
L("loc.first-error-order", {"de.json": {"mod": {"name": ""}}, "aa.json": {"strings": {"k": ""}}})
L("loc.ref-error-order", {"de.json": {"settings": {"missing": {}}}, "aa.json": {"groups": [{}, {}, {}]}})
# resolution
RES = {"en.json": {"mod": {"name": "EN"}, "strings": {"a": "en-a", "b": "en-b"}, "changelog": ["en1"]},
       "en-gb.json": {"mod": {"description": "GB"}, "strings": {"a": "gb-a"}, "settings": {"flag": {"label": "GB flag"}}},
       "de.json": {"mod": {"name": "DE"}, "strings": {"a": "de-a"}, "groups": [{"label": "DE-G1"}, {"actions": {"stop": {"label": "DE stop"}}}],
                   "settings": {"mode": {"options": {"a": "DE-A"}}, "num": {"options": {"2": "DE-2"}}}},
       "de-at.json": {"mod": {"description": "AT"}, "strings": {"b": "at-b"}, "groups": [{"description": "AT-GD", "actions": {"go": {"confirm": "AT?"}}}],
                      "settings": {"flag": {"description": "AT desc"}}, "changelog": ["at1", "at2"]},
       "pt-br.json": {"strings": {"a": "br-a"}}, "zh-hant-tw.json": {"strings": {"a": "tw-a"}}, "zh.json": {"strings": {"a": "zh-a"}},
       "fr.json": {"strings": {"a": "fr-a"}}}
LOCS = ["en", "en-GB", "en-gb", "en-US", "de", "de-at", "de-AT", "de-CH", "pt", "pt-br", "zh-hant-tw", "zh-hant", "fr", "fr-CA", "it", "", "EN", "de-at-x"]
L("loc.resolve", RES, resolve=LOCS, strings=[[k, l, fb] for k in ["a", "b", "c"] for l in LOCS for fb in [None, "FB"]])
L("loc.resolve.none", {}, resolve=["en", "de"], strings=[["a", "en", None], ["a", "de", "FB"]])


# ---- source inference (infer_mod_api_contract.rs:2-41) and enabledWhen evaluation ----
INF = {"settings": {"plain": True, "live": {"value": 1, "applyAt": "live"}, "restart": {"value": 1, "applyAt": "restart"},
                    "rr": {"value": 1, "restartRequired": False}, "rrt": {"value": 1, "restartRequired": True}}}
for i, src in enumerate([None, "", "return {}", "return {on_load = function() end}", "local t = {}\nt.on_update = f",
                         "runtime.require('runtime.lifecycle')", 'runtime.require("runtime.lifecycle")', "runtime.ecs.query()", "runtime.world.x",
                         "runtime.patch.y", "runtime.report_effect()", "on_unload=nil",
                         "on_load=1 runtime.require('game.assets.write')", 'on_load=1 runtime.require("game.assets.write")',
                         "on_load=1 game.assets.update_asset(x)", "on_load=1 game.assets.save_assets()", "on_load=1 game.assets.reset_assets()",
                         "on_load=1 game.assets.create_resource()", "on_load=1 game.assets.create_content()",
                         "-- on_load = 1", "--[[ on_load = 1 ]]", "--[ on_load = 1", "local s = '-- on_load = 1'", 'local s = "on_load = 1"',
                         "local s = 'it\\'s' on_load = 1", "on_load\n=\n1", "ON_LOAD=1", "x.on_load = 1 --[[ game.assets.save_assets( ]]",
                         "game.assets.save_assets ( )", "runtime . ecs . q"]):
    extra = {} if src is None else {"src/mod.lua": src}
    case(f"infer.{i}", None, INF, extra=extra)
case("infer.nested", None, INF, extra={"src/a/b.lua": "on_load=1", "src/z.lua": "x=1"})
case("infer.non-lua", None, INF, extra={"src/a.txt": "on_load=1", "src/b.LUA": "on_load=1"})
case("infer.outside-src", None, INF, extra={"mod.lua": "on_load=1", "lib/a.lua": "on_load=1"})
case("infer.hybrid", None, INF, extra={"src/a.lua": "on_load=1", "src/b.lua": "game.assets.create_content()"})
EWS = {"settings": {"mode": {"value": "a", "options": {"a": "A", "b": "B"}}, "num": 5, "f": 1.5, "flag": True,
                    "a": {"value": True, "enabledWhen": {"setting": "mode", "equals": "a"}},
                    "b": {"value": True, "enabledWhen": {"setting": "num", "equals": 5}},
                    "c": {"value": True, "enabledWhen": {"setting": "a", "equals": True}},
                    "d": {"value": True, "enabledWhen": {"setting": "f", "equals": 1.5}},
                    "e": {"value": True, "enabledWhen": {"setting": "flag", "equals": False}}}}
for i, vals in enumerate([None, {"mode": "b", "num": 5, "f": 1.5, "flag": False, "a": True}, {"mode": "a", "num": 5.0, "f": 1.5, "a": False},
                          {"num": 6, "a": True, "mode": "a"}, {}, {"mode": "a", "a": True, "num": "5", "flag": 0}]):
    case(f"enabled.{i}", None, EWS, enabled_values=vals)
json.dump(cases, open(sys.argv[1], "w"), ensure_ascii=False, indent=0)
print(len(cases), "cases")
