# SPDX-License-Identifier: MIT
# file-level cases for config/compatibility/rules.json, run by the harness's
# "rules-file" mode (SF's own reader over a scratch game directory) and by Kiln's
# rules_file::read_rules over a Kiln-chosen path. "kind" says how the file is set up;
# "text" or "hex" are its bytes; "unix" marks cases a Windows host cannot set up.
import hashlib, json, sys
d = lambda v: json.dumps(v, separators=(",", ":"))
M = {"a": [True, "1.2.0"], "b": [True, "2.0.0"], "c": [False, "1.0.0"], "z": [True, "1.0.0"]}
R = lambda f, s, r="clash": {"first": f, "second": s, "reason": r}
C = lambda i, v="*": {"id": i, "version": v}
cases = []
def case(cid, kind, text=None, raw=None, unix=False):
    c = {"id": cid, "kind": kind, "mods": M}
    data = None
    if raw is not None:
        c["hex"] = raw.hex(); data = raw
    elif text is not None:
        c["text"] = text; data = text.encode()
    if data is not None:
        c["sha256"] = hashlib.sha256(data).hexdigest()
    if unix:
        c["unix"] = True
    cases.append(c)
match = d({"schemaVersion": 1, "conflicts": [R(C("a", "^1"), C("b", ">=2.0.0, <3"))]})
case("file.missing", "missing")
case("file.missing-parent", "missing-parent")
case("file.dangling-symlink", "dangling-symlink", unix=True)
case("file.directory", "directory")
case("file.parent-is-file", "parent-is-file")
case("file.unreadable", "unreadable", match, unix=True)
case("file.empty", "file", "")
case("file.whitespace", "file", " \n")
case("file.invalid-json", "file", '{"schemaVersion":1,"conflicts":[],}')
case("file.truncated", "file", '{"schemaVersion":1,"conflicts":[')
case("file.bom", "file", "\ufeff" + d({"schemaVersion": 1, "conflicts": []}))
case("file.invalid-utf8", "file", raw=b'{"schemaVersion":1,"conflicts":[{"first":{"id":"a","version":"*"},"second":{"id":"b","version":"*"},"reason":"x\xff"}]}')
case("file.invalid-utf8-lines", "file", raw=b'{\n  "schemaVersion": 1,\n  "conflicts": [\n    {"first": {"id": "a", "version": "*"}, "second": {"id": "b", "version": "*"}, "reason": "ok \xc3\xa9 \\" \xe2\x82"}\n  ]\n}\n')
case("file.trailing-garbage", "file", d({"schemaVersion": 1, "conflicts": []}) + " x")
case("file.schemaVersion-2", "file", d({"schemaVersion": 2, "conflicts": []}))
case("file.unknown-key", "file", d({"schemaVersion": 1, "conflicts": [], "extra": True}))
case("file.unknown-rule-key", "file", d({"schemaVersion": 1, "conflicts": [dict(R(C("a"), C("b")), note="x")]}))
case("file.missing-conflicts", "file", d({"schemaVersion": 1}))
case("file.$schema", "file", d({"$schema": "rules.schema.json", "schemaVersion": 1, "conflicts": []}))
case("file.array", "file", "[]")
case("file.bad-range", "file", d({"schemaVersion": 1, "conflicts": [R(C("a", "one"), C("b"))]}))
case("file.valid-empty", "file", d({"schemaVersion": 1, "conflicts": []}))
case("file.valid-default-bytes", "file", '{\n  "schemaVersion": 1,\n  "conflicts": []\n}\n')
case("file.valid-match", "file", match)
case("file.valid-match-crlf", "file", json.dumps(json.loads(match), indent=2).replace("\n", "\r\n"))
case("file.valid-disabled", "file", d({"schemaVersion": 1, "conflicts": [R(C("a"), C("c"))]}))
case("file.valid-later-reason", "file", d({"schemaVersion": 1, "conflicts": [R(C("a"), C("b"), "first"), R(C("b"), C("a"), "last")]}))
json.dump(cases, open(sys.argv[1], "w"), indent=0)
print(len(cases))
