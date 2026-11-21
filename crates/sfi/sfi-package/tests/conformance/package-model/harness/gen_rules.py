# SPDX-License-Identifier: MIT
import json, sys
d = lambda v: json.dumps(v, separators=(",", ":"))
M = {"a": [True, "1.2.0"], "b": [True, "2.0.0"], "c": [False, "1.0.0"], "d": [True, "0.9.0-beta"]}
R = lambda f, s, r="clash": {"first": f, "second": s, "reason": r}
C = lambda i, v="*": {"id": i, "version": v}
cases = []
def case(cid, doc, raw=None):
    cases.append({"id": cid, "rules": raw if raw is not None else d(doc), "mods": M})
case("ok.empty", {"schemaVersion": 1, "conflicts": []})
case("ok.match", {"schemaVersion": 1, "conflicts": [R(C("a", "^1"), C("b", ">=2.0.0, <3"))]})
case("ok.no-match-version", {"schemaVersion": 1, "conflicts": [R(C("a", "^2"), C("b"))]})
case("ok.disabled", {"schemaVersion": 1, "conflicts": [R(C("a"), C("c"))]})
case("ok.missing-mod", {"schemaVersion": 1, "conflicts": [R(C("a"), C("zz"))]})
case("ok.prerelease", {"schemaVersion": 1, "conflicts": [R(C("a"), C("d", "*"))]})
case("ok.prerelease-req", {"schemaVersion": 1, "conflicts": [R(C("a"), C("d", ">=0.9.0-alpha"))]})
case("ok.later-reason-wins", {"schemaVersion": 1, "conflicts": [R(C("a"), C("b"), "first"), R(C("b"), C("a"), "last")]})
case("ok.self", {"schemaVersion": 1, "conflicts": [R(C("a"), C("a"), "self")]})
case("ok.schemaVersion-float", {"schemaVersion": 1.0, "conflicts": []})
case("ok.duplicate-keys", None, '{"schemaVersion":2,"schemaVersion":1,"conflicts":[]}')
case("ok.$schema-unknown", {"$schema": "x", "schemaVersion": 1, "conflicts": []})
case("ok.reason-long", {"schemaVersion": 1, "conflicts": [R(C("a"), C("b"), "r" * 5000)]})
case("ok.case-sensitive", {"schemaVersion": 1, "conflicts": [R(C("A"), C("b"))]})
case("ok.req-exact", {"schemaVersion": 1, "conflicts": [R(C("a", "=1.2.0"), C("b", "2"))]})
case("ok.req-tilde", {"schemaVersion": 1, "conflicts": [R(C("a", "~1.1"), C("b"))]})
for i, doc in enumerate([[], None, "x", {}, {"schemaVersion": 1}, {"conflicts": []}, {"schemaVersion": 2, "conflicts": []},
                         {"schemaVersion": "1", "conflicts": []}, {"schemaVersion": 1, "conflicts": {}},
                         {"schemaVersion": 1, "conflicts": [], "extra": 1}, {"schemaVersion": 1, "conflicts": [None]},
                         {"schemaVersion": 1, "conflicts": [{"first": C("a"), "second": C("b")}]},
                         {"schemaVersion": 1, "conflicts": [{"first": C("a"), "reason": "r"}]},
                         {"schemaVersion": 1, "conflicts": [{"second": C("a"), "reason": "r"}]},
                         {"schemaVersion": 1, "conflicts": [R(C("a"), C("b"), "")]},
                         {"schemaVersion": 1, "conflicts": [R(C("a"), C("b"), 1)]},
                         {"schemaVersion": 1, "conflicts": [dict(R(C("a"), C("b")), extra=1)]},
                         {"schemaVersion": 1, "conflicts": [R({"id": "a"}, C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R({"version": "*"}, C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R(dict(C("a"), extra=1), C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R(C("bad id"), C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R(C(""), C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R(C("a", 1), C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R(C("a", "not semver"), C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R(C("a"), C("b", ""))]},
                         {"schemaVersion": 1, "conflicts": [R(C("zz", "^1"), C("b", ">=1 <2"))]},
                         {"schemaVersion": 1, "conflicts": [R(C("a"), C("b")), R(C("a", "bad"), C("b"))]},
                         {"schemaVersion": 1, "conflicts": [R(C("a", 1), C("b"), "")]},
                         {"schemaVersion": 1, "conflicts": [R("a", C("b"))]}]):
    case(f"bad.{i}", doc)
case("bad.json", None, '{"schemaVersion":1,"conflicts":[],}')
case("bad.empty", None, "")
json.dump(cases, open(sys.argv[1], "w"), indent=0)
print(len(cases))
