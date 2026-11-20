// SPDX-License-Identifier: MIT
// Host test of the review findings on the registry stage's IR reader and composition (registry/ir.cpp, registry/compose.cpp),
// review of e10a6115 (2026-11-20): no exception leaves parse_batch; numbers the field cannot hold are refused before any
// rounding; nesting is limited (run under a 1 MiB stack); old data packs compute their relative ops as the game does (over the
// value below the packs, later pack wins); yield and exclusive count the archive writer's value as base and the mod's own rows
// never trigger yield; old list writes lie below the registry; ids (reserved mod ids, lower case, exact lookup like globs, the
// retired sentinel); edit ordinals; all-or-nothing adds; the exclusive refusal names the exclusive writer; list-op sizes and
// the costs of finals and list removes.
#include <chrono>
#include <cstdio>
#include <string>

#include "../registry/compose.h"

using namespace kc::reg;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) ++g_pass;                                                     \
        else {                                                                  \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);          \
        }                                                                       \
    } while (0)

static Schema schema() {
    Schema s;
    RegistrySpec items;
    items.name = "items";
    FieldSpec ms;
    ms.name = "maxStack", ms.type = FieldType::Int, ms.min = 1, ms.max = 65535;
    FieldSpec cnt;
    cnt.name = "count", cnt.type = FieldType::Int, cnt.max = 1000;  // max only
    FieldSpec raw;
    raw.name = "raw", raw.type = FieldType::Int;  // no range
    FieldSpec pw;
    pw.name = "power", pw.type = FieldType::Float, pw.min = 0, pw.max = 10;
    FieldSpec tags;
    tags.name = "tags", tags.type = FieldType::List, tags.element = FieldType::Id;
    for (const FieldSpec& f : {ms, cnt, raw, pw, tags}) items.fields[f.name] = f;
    s.registries["items"] = items;
    return s;
}

static BaseModel base(int n = 2) {
    BaseModel b;
    for (int i = 0; i < n; ++i) {
        BaseModel::Entry e;
        e.id = "game:it" + std::to_string(i);
        e.fields["maxStack"] = Value::integer(10);
        e.fields["count"] = Value::integer(10);
        e.fields["raw"] = Value::integer(10);
        e.fields["power"] = Value::number(0);
        e.fields["tags"] = Value::of_list({});
        b.registries["items"].push_back(e);
    }
    return b;
}

struct Parsed {
    bool ok = false;
    std::string err;
    Batch b;
    std::vector<Refusal> refused;
};
static Parsed parse(const Schema& s, const std::string& doc) {
    Parsed p;
    try {
        p.ok = parse_batch(doc, s, &p.b, &p.refused, &p.err);
    } catch (...) {
        p.ok = false;
        p.err = "EXCEPTION escaped parse_batch";
    }
    return p;
}
static std::string doc(const std::string& mod, const std::string& stage, const std::string& edits) {
    return R"({"ir": 1, "mod": ")" + mod + R"(", "stage": ")" + stage + R"(", "source": "t", "edits": )" + edits + "}";
}
static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits) { return parse(s, doc(mod, stage, edits)).b; }

static ModInput mod(const std::string& id, std::vector<Batch> batches) {
    ModInput m;
    m.id = id;
    m.batches = std::move(batches);
    return m;
}

static Value result(const Result& r, const std::string& id, const std::string& field) {
    const auto it = r.fields.find({"items", id, field});
    return it == r.fields.end() ? Value{} : it->second.result;
}

static bool refused_with(const Result& r, const char* text) {
    for (const Refusal& x : r.refused)
        if (x.reason.find(text) != std::string::npos) return true;
    return false;
}

static std::string one(const std::string& field, const std::string& op, const std::string& id = "game:it0", const std::string& extra = "") {
    return R"([{"registry": "items", "select": {"id": ")" + id + R"("}, "field": ")" + field + R"(", "op": )" + op + extra + "}]";
}

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    const Schema s = schema();
    const BaseModel b = base();

    // ---- 1: no exception leaves parse_batch ----
    {
        const Parsed a = parse(s, R"({"ir": 1, "mod": "a", "stage": "alter", "modVersion": 5, "edits": []})");
        CHECK(!a.ok && a.err.find("modVersion") != std::string::npos, "a modVersion that is not text refuses the document (no exception)");
        const Parsed c = parse(s, R"({"ir": 1, "mod": "a", "stage": "alter", "source": ["x"], "edits": []})");
        CHECK(!c.ok && c.err.find("source") != std::string::npos, "a source that is not text refuses the document (no exception)");
        const Parsed d = parse(s, R"({"ir": 1.5, "mod": "a", "stage": "alter", "edits": []})");
        CHECK(!d.ok && d.err.find("EXCEPTION") == std::string::npos, "an odd ir number is refused, no exception");
    }

    // ---- 2: numbers the field cannot hold, refused before rounding (UBSan float-cast-overflow is on) ----
    {
        auto refused_or_doc = [&](const std::string& field, const std::string& op) {
            const Parsed p = parse(s, doc("a", "alter", one(field, op)));
            if (!p.ok || !p.refused.empty()) return true;
            const Result r = compose(s, b, {}, {mod("a", {p.b})});
            return r.refused.size() == 1 && r.fields.empty();
        };
        CHECK(refused_or_doc("count", R"({"mul": 1e300})"), "A: an Int field with only a max, mul 1e300: refused");
        CHECK(refused_or_doc("raw", R"({"mul": 1e300})"), "B: an Int field without a range, mul 1e300: refused (cannot be held)");
        CHECK(refused_or_doc("power", R"({"mul": 1e400})"), "C: a Float field, mul 1e400 (not finite): refused");
        CHECK(refused_or_doc("raw", "18446744073709551615"), "D: an Int field, set 2^64-1: refused");
        CHECK(refused_or_doc("raw", "1e300"), "E: an Int field, set 1e300 (whole, out of range): refused");
        CHECK(refused_or_doc("power", R"({"mul": 1e400, "min": 0, "max": 5})"), "F: a clamp does not make a non-finite op acceptable");
        CHECK(refused_or_doc("raw", R"({"add": 1e19})"), "G: a relative result past the whole numbers: refused");
        const Parsed big = parse(s, doc("a", "alter", one("raw", "9223372036854775807")));
        const Result rb = compose(s, b, {}, {mod("a", {big.b})});
        CHECK(big.ok && big.refused.empty() && result(rb, "game:it0", "raw").i == INT64_MAX, "the largest whole number is held");
        const Result over = compose(s, b, {}, {mod("a", {big.b}), mod("b", {batch(s, "b", "alter", one("raw", R"({"add": 1})"))})});
        CHECK(over.refused.size() == 1 && result(over, "game:it0", "raw").i == INT64_MAX, "one more than the largest whole number: refused");
        const Result half = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", one("maxStack", R"({"mul": 0.04})"))})});
        CHECK(half.refused.size() == 1 && refused_with(half, "outside"), "a result that rounds below the range is refused (10 * 0.04 = 0.4 -> 0)");
    }

    // ---- 3: nesting is limited; nothing recurses on an untrusted document ----
    {
        for (int depth : {33, 2000, 200000}) {
            const std::string v = std::string(depth, '[') + std::string(depth, ']');
            const Parsed op = parse(s, doc("a", "alter", one("maxStack", v)));
            CHECK(!op.ok && op.err.find("32") != std::string::npos, ("a value nested " + std::to_string(depth) + " deep refuses the document").c_str());
            const Parsed where = parse(s, doc("a", "alter", R"([{"registry": "items", "select": {"id": "game:it0", "where": {"maxStack": )" + v +
                                                                  R"(}}, "field": "maxStack", "op": 1}])"));
            CHECK(!where.ok, ("a where nested " + std::to_string(depth) + " deep refuses the document").c_str());
        }
        json deep = json::array();
        json* at = &deep;
        for (int i = 0; i < 100000; ++i) {
            at->push_back(json::array());
            at = &at->back();
        }
        json d = {{"ir", 1}, {"mod", "a"}, {"stage", "alter"}, {"edits", json::array({{{"registry", "items"}, {"select", "game:it0"}, {"field", "maxStack"}, {"op", 1}}})}};
        d["edits"][0]["op"] = std::move(deep);
        Batch x;
        std::vector<Refusal> r;
        std::string err;
        bool ok = true;
        try {
            ok = parse_batch(d, s, &x, &r, &err);
        } catch (...) {
            err = "EXCEPTION";
        }
        CHECK(!ok && err.find("32") != std::string::npos, "a deep JSON value handed over as JSON refuses the document");
        const Parsed fine = parse(s, doc("a", "alter", R"([{"registry": "items", "select": {"id": "game:it0", "where": {"maxStack": ">1"}}, "field": "tags", "list": {"append": ["game:a"]}}])"));
        CHECK(fine.ok && fine.refused.empty(), "an ordinary document is well inside the limit");
        const Parsed wl = parse(s, doc("a", "alter", R"([{"registry": "items", "select": {"id": "game:it0", "where": {"maxStack": [1]}}, "field": "maxStack", "op": 1}])"));
        CHECK(wl.ok && wl.refused.size() == 1, "a where condition is a plain value");
    }

    // ---- 4: old data packs as the game applies them: each relative op over the value below the packs, the later pack wins ----
    {
        OldWrite p1;
        p1.kind = WriterKind::OldPack, p1.mod = "p1", p1.who = "p1: content/a.json", p1.field = {"items", "game:it0", "maxStack"};
        p1.op.mul = 2;
        OldWrite p2 = p1;
        p2.mod = "p2", p2.who = "p2: content/a.json", p2.op.mul = 3;
        const Result r = compose(s, b, {p1, p2}, {});
        CHECK(result(r, "game:it0", "maxStack").i == 30, "two old packs' mul 2 and mul 3 on 10: 30 (the game's packs compute from the original)");
        OldWrite sf;
        sf.kind = WriterKind::ArchiveWriter, sf.who = "shroudforge:pkg", sf.field = p1.field;
        sf.op.set = true, sf.op.value = Value::integer(50);
        const Result on = compose(s, b, {sf, p1}, {mod("a", {batch(s, "a", "alter", one("maxStack", R"({"mul": 2})"))})});
        CHECK(result(on, "game:it0", "maxStack").i == 200, "a pack computes from the value the game loaded (the archive's 50 -> 100); a registry mul composes on it (200)");
        OldWrite patch;
        patch.kind = WriterKind::OldPatch, patch.mod = "bp", patch.who = "bp/k", patch.field = p1.field;
        patch.op.add = 5;
        const Result pp = compose(s, b, {patch, p1, p2}, {});
        CHECK(result(pp, "game:it0", "maxStack").i == 45, "boot patches are below the packs: (10 + 5) * 3");
    }

    // ---- 5: yield and exclusive count the archive writer's value as base; the mod's own rows never trigger yield ----
    {
        OldWrite sf;
        sf.kind = WriterKind::ArchiveWriter, sf.who = "shroudforge:pkg", sf.field = {"items", "game:it0", "maxStack"};
        sf.op.set = true, sf.op.value = Value::integer(50);
        const Result h = compose(s, b, {sf}, {mod("a", {batch(s, "a", "alter", one("maxStack", "7", "game:it0", R"(, "yield": true)"))})});
        CHECK(result(h, "game:it0", "maxStack").i == 7, "H: yield over an archive-writer value applies (it counts as base)");
        const Result i = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:it0"}, "field": "maxStack", "op": 7},
                                                                                 {"registry": "items", "select": {"id": "game:it0"}, "field": "maxStack", "op": 9, "yield": true}])")})});
        CHECK(result(i, "game:it0", "maxStack").i == 9, "I: the mod's own earlier edit does not make it yield");
        const Result ex = compose(s, b, {sf}, {mod("a", {batch(s, "a", "alter", one("maxStack", "7", "game:it0", R"(, "exclusive": true)"))})});
        CHECK(ex.refused.empty() && result(ex, "game:it0", "maxStack").i == 7, "an exclusive edit over an archive-writer value applies (base)");
        OldWrite own;
        own.kind = WriterKind::OldPack, own.mod = "a", own.who = "a: content/x.json", own.field = sf.field;
        own.op.set = true, own.op.value = Value::integer(20);
        const Result oy = compose(s, b, {own}, {mod("a", {batch(s, "a", "alter", one("maxStack", "9", "game:it0", R"(, "yield": true)"))})});
        CHECK(result(oy, "game:it0", "maxStack").i == 9, "the mod's own old pack does not make its registry edit yield");
        OldWrite other = own;
        other.mod = "z", other.who = "z: content/x.json";
        const Result zy = compose(s, b, {other}, {mod("a", {batch(s, "a", "alter", one("maxStack", "9", "game:it0", R"(, "yield": true)"))})});
        CHECK(result(zy, "game:it0", "maxStack").i == 20, "another mod's old pack makes it yield");
        // 12: the exclusive refusal names the exclusive writer
        const Result l = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", one("maxStack", "5", "game:it0", R"(, "exclusive": true)"))}),
                                            mod("b", {batch(s, "b", "alter", one("maxStack", "5"))}),
                                            mod("c", {batch(s, "c", "alter", one("maxStack", "6"))})});
        CHECK(l.refused.size() == 1 && l.refused[0].mod == "c" && l.refused[0].reason.find("Conflict (exclusive): a ") != std::string::npos,
              "L: c's refusal names a (the exclusive edit), not b");
    }

    // ---- 6: old list writes lie below the registry and are reported ----
    {
        OldWrite p;
        p.kind = WriterKind::OldPack, p.mod = "old", p.who = "old: content/l.json", p.field = {"items", "game:it0", "tags"};
        p.list = ListOp::Append, p.list_values = {Value::text("game:a")};
        const Result r = compose(s, b, {p}, {mod("n", {batch(s, "n", "alter", R"([{"registry": "items", "select": {"id": "game:it0"}, "field": "tags", "list": {"append": ["game:b"]}}])")})});
        const Value v = result(r, "game:it0", "tags");
        CHECK(v.list.size() == 2 && v.list[0].t == "game:a" && v.list[1].t == "game:b", "an old append below a registry append: both, old first");
        const auto& chain = r.fields.at({"items", "game:it0", "tags"}).chain;
        CHECK(chain.size() == 2 && chain[0].kind == WriterKind::OldPack && chain[0].list == ListOp::Append && r.lines.size() == 1, "the old list row is in the chain, the composition reported");
        OldWrite clr = p;
        clr.list = ListOp::Clear, clr.list_values.clear();
        BaseModel withtags = b;
        withtags.registries["items"][0].fields["tags"] = Value::of_list({Value::text("game:x")});
        const Result c = compose(s, withtags, {clr}, {mod("n", {batch(s, "n", "alter", R"([{"registry": "items", "select": {"id": "game:it0"}, "field": "tags", "list": {"append": ["game:b"]}}])")})});
        CHECK(result(c, "game:it0", "tags").list.size() == 1, "an old clear, then a registry append");
    }

    // ---- 7: ids ----
    {
        CHECK(!parse(s, doc("game", "add", R"([{"registry": "items", "entry": "add", "id": "game:foo"}])")).ok, "K: a batch of mod \"game\" is refused");
        CHECK(!parse(s, doc("kiln", "alter", one("maxStack", "1"))).ok, "a batch of mod \"kiln\" is refused");
        CHECK(!parse(s, doc("Big Stacks", "alter", one("maxStack", "1"))).ok, "a mod id that is no Kiln mod id is refused");
        ModInput g;
        g.id = "game";
        g.code = [](Stage st, const View&, Batch* out) {
            if (st != Stage::Add) return;
            Edit e;
            e.registry = "items", e.entry = EntryOp::Add, e.add_id = "game:foo";
            out->edits.push_back(e);
        };
        const Result gr = compose(s, b, {}, {g});
        CHECK(gr.added.empty() && gr.refused.size() == 1, "code of a mod named game cannot mint game: ids");
        const Parsed up = parse(s, doc("a", "add", R"([{"registry": "items", "entry": "add", "id": "a:Crate"}, {"registry": "items", "entry": "add", "id": "a:crate"}])"));
        CHECK(up.b.edits.size() == 1 && up.refused.size() == 1 && up.refused[0].reason.find("lower case") != std::string::npos, "J: an added id is lower case");
        const Result j = compose(s, b, {}, {mod("a", {up.b, batch(s, "a", "alter", one("maxStack", "5", "A:CRATE"))})});
        CHECK(j.refused.empty() && result(j, "a:crate", "maxStack").i == 5, "an exact id selects without case, as a glob does");
        const Result ret = compose(s, b, {}, {mod("retired", {batch(s, "retired", "alter", R"([{"registry": "items", "entry": "remove", "select": {"id": "game:it0"}}])")}),
                                              mod("z", {batch(s, "z", "alter", one("maxStack", "5"))})});
        CHECK(ret.refused.empty() && ret.lines.size() == 1 && ret.lines[0].kind == LineKind::Dropped, "a mod named retired is a mod: an edit after its remove is dropped");
    }

    // ---- 12: edit ordinals and all-or-nothing adds ----
    {
        const Parsed n = parse(s, doc("a", "alter", R"([{"n": 4294967297, "registry": "items", "select": {"id": "game:it0"}, "field": "maxStack", "op": 5},
                                                         {"n": 1, "registry": "items", "select": {"id": "game:it0"}, "field": "maxStack", "op": 5},
                                                         {"n": 1, "registry": "items", "select": {"id": "game:it1"}, "field": "maxStack", "op": 6}])"));
        CHECK(n.b.edits.size() == 1 && n.refused.size() == 2, "an oversize n and a second n 1 are refused");
        bool named = false;
        for (const Refusal& x : n.refused) named = named || (x.n == 0 && x.reason.find("4294967295") != std::string::npos);
        CHECK(named, "the oversize n's refusal carries its position, not a truncated n");
        ModInput m;
        m.id = "a";
        m.code = [](Stage st, const View&, Batch* out) {
            if (st != Stage::Add) return;
            Edit e;
            e.registry = "items", e.entry = EntryOp::Add, e.add_id = "a:half";
            e.add_fields["maxStack"] = Value::integer(5);
            e.add_fields["count"] = Value::integer(5000);  // over count's max
            out->edits.push_back(e);
        };
        const Result r = compose(s, b, {}, {m});
        CHECK(r.added.empty() && r.fields.empty() && r.refused.size() == 1, "an add with one bad field is refused whole: no entry, no field");
    }

    // ---- 11: list-op sizes and the costs ----
    {
        std::string vals = "[";
        for (int i = 0; i < 4097; ++i) vals += (i ? ",\"m:v" : "\"m:v") + std::to_string(i) + "\"";
        vals += "]";
        const Parsed big = parse(s, doc("a", "alter", R"([{"registry": "items", "select": {"id": "game:it0"}, "field": "tags", "list": {"append": )" + vals + "}}]"));
        CHECK(big.b.edits.empty() && big.refused.size() == 1 && big.refused[0].reason.find("4096") != std::string::npos, "a list op of 4,097 values is refused");
        std::string v4 = "[";
        for (int i = 0; i < 4096; ++i) v4 += (i ? ",\"m:v" : "\"m:v") + std::to_string(i) + "\"";
        v4 += "]";
        const auto t0 = std::chrono::steady_clock::now();
        const Result lr = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:it0"}, "field": "tags", "list": {"append": )" + v4 +
                                                                                    R"(}}, {"registry": "items", "select": {"id": "game:it0"}, "field": "tags", "list": {"remove": )" + v4 + "}}]")})});
        const double list_ms = ms_since(t0);
        std::printf("registry-review-test: 4,096 appended and removed in %.1f ms\n", list_ms);
        CHECK(result(lr, "game:it0", "tags").list.empty() && list_ms < 500, "a remove of 4,096 values is hashed (well under 500 ms with sanitizers)");

        const BaseModel bb = base(4000);
        std::vector<ModInput> mods;
        for (int i = 0; i < 20; ++i) {
            const std::string id = "m" + std::to_string(i);
            mods.push_back(mod(id, {batch(s, id, "alter", R"([{"registry": "items", "select": {"id": "*"}, "field": "maxStack", "op": {"add": 1}}])")}));
        }
        for (int i = 0; i < 60; ++i) {
            ModInput m;
            m.id = "c" + std::to_string(i);
            m.code = [](Stage, const View&, Batch*) {};
            mods.push_back(m);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const Result fr = compose(s, bb, {}, mods);
        const double final_ms = ms_since(t1);
        std::printf("registry-review-test: 4,000 entries x 20 alter mods + 60 code mods in %.1f ms\n", final_ms);
        CHECK(fr.fields.size() == 4000 && result(fr, "game:it7", "maxStack").i == 30 && final_ms < 3000, "finals run on overlays, not on copies of the whole state");

        // the plan's budget: 4,096 old changes + 50,000 registry edits (P1-PLAN.md 5.3: under 100 ms on this PC, without sanitizers)
        std::vector<OldWrite> old;
        for (int i = 0; i < 4096; ++i) {
            OldWrite w;
            w.kind = WriterKind::OldPack, w.mod = "old", w.who = "old: content/a.json", w.field = {"items", "game:it" + std::to_string(i % 4000), "count"};
            w.op.set = true, w.op.value = Value::integer(i % 900);
            old.push_back(w);
        }
        std::vector<ModInput> many;
        for (int m = 0; m < 25; ++m) {
            std::string edits = "[";
            for (int e = 0; e < 2000; ++e)
                edits += std::string(e ? "," : "") + R"({"registry": "items", "select": {"id": "game:it)" + std::to_string((m * 2000 + e) % 4000) +
                         R"("}, "field": ")" + (e % 2 ? "maxStack" : "power") + R"(", "op": )" + (e % 2 ? R"({"add": 1})" : "1.5") + "}";
            edits += "]";
            const std::string id = "r" + std::to_string(m);
            many.push_back(mod(id, {batch(s, id, "alter", edits)}));
        }
        const auto t2 = std::chrono::steady_clock::now();
        const Result br = compose(s, bb, old, many);
        const double budget_ms = ms_since(t2);
        std::printf("registry-review-test: 4,096 old changes + 50,000 registry edits composed in %.1f ms (this build's flags)\n", budget_ms);
        CHECK(br.refused.empty() && budget_ms < 5000, "the plan's load composes (time printed; the 100 ms budget is for a build without sanitizers)");
    }

    std::printf("registry-review-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
