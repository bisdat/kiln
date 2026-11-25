// SPDX-License-Identifier: MIT
// Host test of the second review of the registry stage (dev31/registry-b at ee378645, 2026-11-21): the reference check never
// removes a game entry and completes in any order with bounded cause chains; per-field ops and fixed-length lists; a cap on
// every list; raw paths against Kiln fields (normalised cover keys, elements of a covered list, old writes on a covered
// member); stage callbacks read the previous stage's resolved view; registry.json refusals name registry, entry and field,
// writers per row are capped; the plan's load composes clearly under 100 ms at -O2 (KILN_BUDGET builds assert it).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

#include "../registry/report.h"

using namespace kc::registry;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) ++g_pass;                                                     \
        else {                                                                  \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);          \
        }                                                                       \
    } while (0)

static FieldSpec fs(const std::string& n, FieldType t, FieldOps ops = FieldOps::Any) {
    FieldSpec f;
    f.name = n, f.type = t, f.ops = ops, f.applies = Applies::Now;
    return f;
}

static Value elem(std::vector<std::pair<std::string, Value>> kv) {
    std::sort(kv.begin(), kv.end(), [](auto& a, auto& b) { return a.first < b.first; });
    Value v;
    v.kind = Value::Struct;
    for (auto& [k, x] : kv) v.keys.push_back(k), v.list.push_back(x);
    return v;
}

// drop_leaves: the drop table's weight and count.max leaves too, which an item removal needs to judge a group (slice 5)
static Schema schema(bool drop_leaves = false) {
    Schema s;
    RegistrySpec items;
    items.name = "items", items.raw = true;
    FieldSpec ms = fs("maxStack", FieldType::Int, FieldOps::Number);
    ms.min = 1, ms.max = 65535;
    FieldSpec rar = fs("rarity", FieldType::Int, FieldOps::Set);
    rar.min = 0, rar.max = 4;
    FieldSpec hid = fs("hidden", FieldType::Bool, FieldOps::None);
    FieldSpec perks = fs("perks", FieldType::List, FieldOps::List);
    perks.element = FieldType::Id, perks.max_items = 5, perks.ref = "perks";
    FieldSpec up = fs("upgrade", FieldType::Id);
    up.ref = "items";
    FieldSpec xs = fs("xs", FieldType::List, FieldOps::List);  // no maxItems: the stage's own cap holds
    xs.element = FieldType::Int;
    FieldSpec ys = fs("ys", FieldType::List, FieldOps::List);
    ys.element = FieldType::Int, ys.max_items = 32;
    for (const FieldSpec& f : {ms, rar, hid, perks, up, xs, ys}) items.fields[f.name] = f;
    RegistrySpec recipes;
    recipes.name = "recipes";
    FieldSpec inputs = fs("inputs", FieldType::List, FieldOps::List);
    inputs.max_items = 32;
    FieldSpec ii = fs("inputs[].item", FieldType::Id, FieldOps::Set);
    ii.ref = "items";
    FieldSpec ic = fs("inputs[].count", FieldType::Int, FieldOps::Number);
    ic.min = 1, ic.max = 65535;
    for (const FieldSpec& f : {inputs, ii, ic}) recipes.fields[f.name] = f;
    RegistrySpec drops;
    drops.name = "drops", drops.add = drops.remove = false;
    FieldSpec en = fs("entries", FieldType::List, FieldOps::List);
    en.max_items = 4096;
    FieldSpec ei = fs("entries[].item", FieldType::Id, FieldOps::None);
    ei.ref = "items", ei.read_only = true;
    FieldSpec ech = fs("entries[].chance", FieldType::Float, FieldOps::Number);
    ech.min = 0, ech.max = 1;
    for (const FieldSpec& f : {en, ei, ech}) drops.fields[f.name] = f;
    if (drop_leaves) {
        FieldSpec ew = fs("entries[].weight", FieldType::Float, FieldOps::Number);
        ew.min = 0;
        FieldSpec emax = fs("entries[].count.max", FieldType::Int, FieldOps::Number);
        emax.min = 0, emax.max = 65535;
        for (const FieldSpec& f : {ew, emax}) drops.fields[f.name] = f;
    }
    RegistrySpec bal;
    bal.name = "balance", bal.add = bal.remove = false;
    FieldSpec xp = fs("xpPerLevel", FieldType::List, FieldOps::Indexed);
    xp.element = FieldType::Int, xp.min = 0, xp.max = 1e8, xp.fixed_length = 5;
    bal.fields[xp.name] = xp;
    s.registries = {{"items", items}, {"recipes", recipes}, {"drops", drops}, {"balance", bal}};
    link_elements(&s);
    return s;
}

static BaseModel base() {
    BaseModel b;
    for (const char* id : {"game:wood", "game:stone"}) {
        BaseModel::Entry e;
        e.id = id;
        e.fields["maxStack"] = Value::integer(10);
        e.fields["rarity"] = Value::integer(1);
        e.fields["perks"] = Value::of_list({Value::text("game:p1"), Value::text("game:p2"), Value::text("game:p3")});
        e.fields["xs"] = Value::of_list({});
        e.fields["ys"] = Value::of_list({});
        b.registries["items"].push_back(e);
    }
    for (int i = 0; i < 4266; ++i) {  // 4,268 items in all, as the server build has
        BaseModel::Entry e;
        e.id = "game:material_t1_something_" + std::to_string(100000 + i);
        e.fields["maxStack"] = Value::integer(10);
        e.fields["ys"] = Value::of_list({});
        b.registries["items"].push_back(e);
    }
    BaseModel::Entry r1;
    r1.id = "game:r1";
    r1.fields["inputs"] = Value::of_list({elem({{"item", Value::text("game:wood")}, {"count", Value::integer(1)}})});
    BaseModel::Entry r2;
    r2.id = "game:r2";
    r2.fields["inputs"] = Value::of_list({elem({{"item", Value::text("game:ghost")}, {"count", Value::integer(1)}})});
    b.registries["recipes"] = {r1, r2};
    BaseModel::Entry d;
    d.id = "game:table1";
    d.fields["entries"] = Value::of_list({elem({{"item", Value::text("game:wood")}, {"chance", Value::number(0.5)}}), elem({{"item", Value::text("game:stone")}, {"chance", Value::number(0.5)}})});
    b.registries["drops"] = {d};
    BaseModel::Entry x;
    x.id = "game:balance";
    x.fields["xpPerLevel"] = Value::of_list({Value::integer(100), Value::integer(200), Value::integer(300), Value::integer(400), Value::integer(500)});
    b.registries["balance"] = {x};
    b.raw = [](const std::string&, const std::string&, const std::string& key, std::string*) -> std::optional<RawMember> {
        RawMember m;
        m.spec = fs(key, FieldType::Int);
        m.value = Value::integer(10);
        return m;
    };
    return b;
}

struct Parsed {
    Batch b;
    std::vector<Refusal> refused;
};
static Parsed parse(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits) {
    Parsed p;
    std::string err;
    if (!parse_batch(R"({"ir": 1, "mod": ")" + mod + R"(", "stage": ")" + stage + R"(", "source": "content/)" + mod + R"(.json", "edits": )" + edits + "}", s, &p.b, &p.refused, &err))
        std::printf("doc: %s\n", err.c_str());
    return p;
}
static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits) { return parse(s, mod, stage, edits).b; }
static ModInput mod(const std::string& id, std::vector<Batch> b) {
    ModInput m;
    m.id = id;
    m.batches = std::move(b);
    return m;
}
static bool removed(const Result& r, const std::string& reg, const std::string& id) {
    for (const EntryRef& e : r.removed)
        if (e.registry == reg && e.id == id) return true;
    return false;
}
static const Refusal* refusal(const Result& r, const std::string& text) {
    for (const Refusal& x : r.refused)
        if (x.reason.find(text) != std::string::npos) return &x;
    return nullptr;
}
static Value result(const Result& r, const std::string& reg, const std::string& id, const std::string& field) {
    const auto it = r.fields.find({reg, id, field});
    return it == r.fields.end() ? Value{} : it->second.result;
}
static double ms_since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

int main() {
    const Schema s = schema();
    const BaseModel b = base();

    // ---- 1: the reference check never removes or hides a game entry ----
    {
        const Result none = compose(s, b, {}, {});
        CHECK(none.removed.empty() && none.lines.empty() && none.refused.empty(), "no mods: a vanilla recipe naming an id the base has not got stays (the model may be partial)");
        // slice 5 (6fa979f5, ruling 2026-11-23 4:35 PM): an item removal hides the item and the recipes naming it; a drop table keeps
        // its physical slots and indexes, the slots naming the item are disabled (chance 0, weight 0). The table's group spans
        // (BaseModel drop_groups) must be known and every group must keep a stack that can spawn, else the removal is refused whole.
        const char* remove_wood = R"([{"registry": "items", "entry": "remove", "select": {"id": "game:wood"}}])";
        const Schema sd = schema(true);
        BaseModel bd = base();
        for (BaseModel::Entry& t : bd.registries["drops"]) {
            for (Value& el : t.fields["entries"].list)
                el = elem({{"item", *el.leaf("item")}, {"chance", *el.leaf("chance")}, {"weight", Value::number(1)}, {"count.max", Value::integer(1)}});
            t.drop_groups = {{0, 2}};
        }
        const Result rm = compose(sd, bd, {}, {mod("a", {batch(sd, "a", "alter", remove_wood)})});
        const Value rmd = result(rm, "drops", "game:table1", "entries");
        auto leaf = [&](size_t i, const char* k) { return rmd.list.size() > i && rmd.list[i].leaf(k) ? rmd.list[i].leaf(k)->as_double() : -1.0; };
        CHECK(removed(rm, "items", "game:wood") && removed(rm, "recipes", "game:r1") && !removed(rm, "drops", "game:table1") && rm.removed.size() == 2 &&
                  rmd.list.size() == 2 && rmd.list[0].leaf("item")->t == "game:wood" && leaf(0, "chance") == 0 && leaf(0, "weight") == 0 &&
                  rmd.list[1].leaf("item")->t == "game:stone" && leaf(1, "chance") == 0.5 && leaf(1, "weight") == 1 && !removed(rm, "recipes", "game:r2"),
              "a mod's remove: the recipe naming it goes too; a drop table (its entries are never removed) keeps its slots and disables the one naming it");
        const Result ungrouped = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", remove_wood)})});
        CHECK(!removed(ungrouped, "items", "game:wood") && !removed(ungrouped, "recipes", "game:r1") && ungrouped.removed.empty() &&
                  refusal(ungrouped, "drop grouping of game:table1 is not proven on this build") &&
                  result(ungrouped, "drops", "game:table1", "entries").kind == Value::None,
              "without the table's group spans the item removal is refused whole: nothing hidden, no slot disabled");
        Refusal pre;
        pre.mod = "a", pre.stage = Stage::Add, pre.entry = "items|a:gem", pre.reason = "Unmapped: items.power";
        ModInput a = mod("a", {batch(s, "a", "alter", R"([{"registry": "recipes", "select": {"id": "game:r1"}, "field": "inputs", "list": {"append": [{"item": "a:gem", "count": 1}]}}])")});
        a.refused = {pre};
        const Result dep = compose(s, b, {}, {a});
        const Value in = result(dep, "recipes", "game:r1", "inputs");
        CHECK(!removed(dep, "recipes", "game:r1") && in.kind != Value::List && refusal(dep, "inputs[1].item a:gem refused: Unmapped"),
              "an edit bringing a refused mod entry into a game recipe is refused there; the recipe keeps its own inputs");
        // a chain of 100 added entries in the unfavourable order: every one refused (the fixpoint completes)
        std::string add = "[";
        for (int i = 0; i < 100; ++i)
            add += std::string(i ? "," : "") + R"({"registry": "items", "entry": "add", "id": "a:e)" + std::to_string(i) + R"(", "fields": {"upgrade": ")" +
                   (i == 99 ? std::string("a:nothere") : "a:e" + std::to_string(i + 1)) + R"("}})";
        add += "]";
        const Result chain = compose(s, b, {}, {mod("a", {batch(s, "a", "add", add)})});
        CHECK(chain.added.empty() && chain.refused.size() == 100, "a chain of 100 references in the unfavourable order: all 100 refused");
        // 2,000 in the favourable order: the cause chain stays bounded
        std::string add2 = "[";
        for (int i = 0; i < 2000; ++i)
            add2 += std::string(i ? "," : "") + R"({"registry": "items", "entry": "add", "id": "a:f)" + std::to_string(i) + R"(", "fields": {"upgrade": ")" +
                    (i == 0 ? std::string("a:nothere") : "a:f" + std::to_string(i - 1)) + R"("}})";
        add2 += "]";
        const auto t0 = std::chrono::steady_clock::now();
        const Result long_chain = compose(s, b, {}, {mod("a", {batch(s, "a", "add", add2)})});
        const double chain_ms = ms_since(t0);
        size_t longest = 0, total = 0;
        for (const Refusal& x : long_chain.refused) longest = std::max(longest, x.reason.size()), total += x.reason.size();
        std::printf("registry-review2-test: 2,000-entry reference chain in %.1f ms, longest reason %zu bytes, all %zu bytes\n", chain_ms, longest, total);
        CHECK(long_chain.refused.size() == 2000 && longest <= 1024 && total <= 2000 * 1024, "cause chains are cut to 1 KiB, keeping the first links and the root cause");
        const Refusal* last = refusal(long_chain, "items a:f1999 refused: upgrade a:f1998 refused");
        CHECK(last && last->reason.find("a:nothere does not exist") != std::string::npos, "...and a cut chain still names its root cause");
        // a conflict line whose winner the check takes out does not stay
        const Result stale = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "recipes", "select": {"id": "game:r1"}, "field": "inputs", "list": {"replace": [{"item": "game:stone", "count": 2}]}}])")}),
                                                mod("b", {batch(s, "b", "alter", R"([{"registry": "recipes", "select": {"id": "game:r1"}, "field": "inputs", "list": {"replace": [{"item": "b:nothere", "count": 3}]}}])")})});
        bool stale_line = false;
        for (const Line& l : stale.lines) stale_line = stale_line || (l.winner == "b" && l.field.id == "game:r1");
        CHECK(!stale_line && refusal(stale, "b:nothere does not exist"), "a line naming an edit the reference check took out is taken out with it");
    }

    // ---- review nit 9: the text and the JSON form of one document meet the nesting limit alike ----
    {
        bool same = true;
        for (int d = 26; d <= 34; ++d) {
            const std::string v = std::string(d, '[') + std::string(d, ']');
            const std::string text = R"({"ir": 1, "mod": "a", "stage": "alter", "edits": [{"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": )" + v + "}]}";
            Batch x, y;
            std::vector<Refusal> rx, ry;
            std::string ex, ey;
            const bool a = parse_batch(text, s, &x, &rx, &ex);
            const bool j = parse_batch(json::parse(text), s, &y, &ry, &ey);
            same = same && a == j && ex == ey && rx.size() == ry.size();
        }
        CHECK(same, "nesting: text and JSON give the same answer at every depth around the limit");
    }

    // ---- 2: per-field ops, fixed length, read-only leaves on a new element ----
    {
        const Parsed p = parse(s, "a", "alter", R"([
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel", "list": {"append": [1]}},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel", "list": {"remove": [100]}},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel", "op": []},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel[2]", "op": {"add": 1}},
            {"registry": "items", "select": {"id": "game:wood"}, "field": "rarity", "op": {"add": 1}},
            {"registry": "items", "select": {"id": "game:wood"}, "field": "rarity", "op": 3},
            {"registry": "items", "select": {"id": "game:wood"}, "field": "hidden", "op": true},
            {"registry": "drops", "select": {"id": "game:table1"}, "field": "entries", "list": {"append": [{"item": "game:stone", "chance": 0.1}]}},
            {"registry": "drops", "select": {"id": "game:table1"}, "field": "entries[0].item", "op": "game:stone"},
            {"registry": "drops", "select": {"id": "game:table1"}, "field": "entries[0].chance", "op": {"mul": 2}}
        ])");
        CHECK(p.b.edits.size() == 4 && p.refused.size() == 6, "ops: indexed lists take element edits only, set fields no relative op, none fields nothing, read-only leaves no edit");
        const Parsed past = parse(s, "b", "alter", R"([{"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel[5]", "op": 1}])");
        const Result r = compose(s, b, {}, {mod("a", {p.b}), mod("b", {past.b})});
        const Value xp = result(r, "balance", "game:balance", "xpPerLevel");
        CHECK(xp.list.size() == 5 && xp.list[2].i == 301 && past.refused.size() == 1 && past.refused[0].reason.find("no element 5") != std::string::npos,
              "a fixed-length list keeps its length; an element past it is refused (by the reader: the length is fixed)");
        const Value en = result(r, "drops", "game:table1", "entries");
        CHECK(en.list.size() == 3 && en.list[2].leaf("item")->t == "game:stone" && en.list[0].leaf("chance")->d == 1.0, "a drop entry is appended with its (read-only afterwards) item");
        ModInput code;
        code.id = "c";
        code.code = [](Stage st, const View&, Batch* out) {
            if (st != Stage::Alter) return;
            Edit e;
            e.registry = "balance", e.select.id = "game:balance", e.field = "xpPerLevel", e.list = ListOp::Clear;
            out->edits.push_back(e);
        };
        const Result cr = compose(s, b, {}, {code});
        CHECK(cr.refused.size() == 1 && result(cr, "balance", "game:balance", "xpPerLevel").kind == Value::None, "code edits are held to the field's ops too");
    }

    // ---- 3: every list is capped ----
    {
        std::string vals = "[";
        for (int i = 0; i < 4096; ++i) vals += std::string(i ? "," : "") + std::to_string(i % 10);
        vals += "]";
        std::string e = "[";
        for (int i = 0; i < 40; ++i) e += std::string(i ? "," : "") + R"({"registry": "items", "select": {"id": "game:wood"}, "field": "xs", "list": {"append": )" + vals + "}}";
        e += "]";
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", e)})});
        CHECK(result(r, "items", "game:wood", "xs").list.size() == 4096 && r.refused.size() == 39 && refusal(r, "4096"),
              "a list without maxItems holds at most 4,096 values: the appends past it are refused, reported");
        std::string v1024 = "[";
        for (int i = 0; i < 1024; ++i) v1024 += std::string(i ? "," : "") + std::to_string(i % 10);
        v1024 += "]";
        const Batch over = batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "*"}, "field": "ys", "list": {"append": )" + v1024 + "}}]");
        const auto t0 = std::chrono::steady_clock::now();
        const Result r4 = compose(s, b, {}, {mod("a", {over})});
        const double over_ms = ms_since(t0);
        std::printf("registry-review2-test: one 1,024-value append over 4,268 entries (maxItems 32) refused in %.1f ms\n", over_ms);
        CHECK(r4.refused.size() == 1 && r4.fields.empty() && over_ms < 200, "a too-long append over a glob is refused at its first entry without copying it per entry");
    }

    // ---- 4: raw paths against Kiln fields ----
    {
        BaseModel cover = b;
        cover.raw_cover["items"]["maxStackSize"] = {"maxStack", false};  // the regmap's spelling, not the key
        const Result t4 = compose(s, cover, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": 7}])")}),
                                                 mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "raw": {"maxStackSize": 9}}])")})});
        CHECK(result(t4, "items", "game:wood", "maxStack").i == 9 && !t4.fields.count({"items", "game:wood", raw_field("maxstacksize")}), "a cover key in any spelling: one chain");
        BaseModel lc = b;
        lc.raw_cover["items"]["perkIds"] = {"perks", false};
        const Result t5 = compose(s, lc, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "perks", "list": "clear"}])")}),
                                              mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "raw": {"perkIds[2]": "game:p9"}}])")})});
        CHECK(!t5.fields.count({"items", "game:wood", raw_field("perkids[2]")}) && refusal(t5, "no element 2"), "a raw element of a covered list is an element edit on the list's chain");
        const Result t5b = compose(s, lc, {}, {mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "raw": {"perkIds[1]": "game:p9"}}])")})});
        const Value pk = result(t5b, "items", "game:wood", "perks");
        CHECK(pk.list.size() == 3 && pk.list[1].t == "game:p9", "...which applies where the element is there");
        const Result t5c = compose(s, lc, {}, {mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "raw": {"perkIds[1].x": 1}}])")})});
        CHECK(t5c.refused.size() == 1 && t5c.fields.empty(), "a member inside a covered list's element is refused (edit the Kiln field)");
        BaseModel oc = b;
        oc.raw_cover["items"]["maxstacksize"] = {"maxStack", false};
        OldWrite w;
        w.kind = WriterKind::OldPack, w.mod = "old", w.who = "old: content/a.json", w.field = {"items", "game:wood", raw_field("maxstacksize")};
        w.op.set = true, w.op.value = Value::integer(100);
        const Result t6 = compose(s, oc, {w}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": {"mul": 4}}])")})});
        CHECK(result(t6, "items", "game:wood", "maxStack").i == 400 && !t6.fields.count({"items", "game:wood", raw_field("maxstacksize")}), "an old write on a covered member is on the field's chain");
    }

    // ---- 5: a stage callback reads the previous stage's resolved view ----
    {
        int seen = -1;
        ModInput c;
        c.id = "c";
        c.code = [&](Stage st, const View& v, Batch*) {
            if (st != Stage::Alter) return;
            const auto x = v.get({"items", "game:wood", "maxStack"});
            seen = x ? static_cast<int>(x->i) : -1;
        };
        compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": 55}])")}), c});
        CHECK(seen == 10, "an alter callback reads the view as add left it, not an earlier mod's alter");
        int fin = -1;
        ModInput d = mod("d", {batch(s, "d", "final", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": 60}])")});
        d.code = [&](Stage st, const View& v, Batch*) {
            if (st != Stage::Final) return;
            const auto x = v.get({"items", "game:wood", "maxStack"});
            fin = x ? static_cast<int>(x->i) : -1;
        };
        compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": 55}])")}), d});
        CHECK(fin == 55, "a final callback reads the view as alter left it, not its own final edits");
    }

    // ---- 6: registry.json ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": {"mul": 100000}}])")})});
        const json doc = report_json(s, r, {});
        CHECK(doc["refused"].size() == 1 && doc["refused"][0].value("registry", "") == "items" && doc["refused"][0].value("id", "") == "game:wood" &&
                  doc["refused"][0].value("field", "") == "maxStack",
              "a refused row names registry, entry and field");
        std::string e = "[";
        for (int i = 0; i < 3000; ++i) e += std::string(i ? "," : "") + R"({"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": {"add": 1}})";
        e += "]";
        const Result many = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", e)})});
        const json d2 = report_json(s, many, {});
        CHECK(d2["fields"][0]["writers"].size() == 64 && d2["fields"][0].value("writersLeftOut", 0) == 2936 && d2["truncated"].value("writers", 0) == 2936 &&
                  d2["fields"][0]["writers"].back().value("value", 0) == 3010,
              "writers per row: the last 64, the earlier ones counted");
    }

    // ---- 7: the plan's load (4,096 old changes + 50,000 edits), and globs that match nothing ----
    {
        std::vector<OldWrite> old;
        for (int i = 0; i < 4096; ++i) {
            OldWrite w;
            w.kind = WriterKind::OldPack, w.mod = "old", w.who = "old: content/a.json";
            w.field = {"items", "game:material_t1_something_" + std::to_string(100000 + i % 4000), "maxStack"};
            w.op.set = true, w.op.value = Value::integer(1 + i % 900);
            old.push_back(w);
        }
        std::vector<ModInput> mods;
        for (int m = 0; m < 25; ++m) {
            std::string edits = "[";
            for (int k = 0; k < 2000; ++k)
                edits += std::string(k ? "," : "") + R"({"registry": "items", "select": {"id": "game:material_t1_something_)" + std::to_string(100000 + (m * 2000 + k) % 4000) +
                         R"("}, "field": "maxStack", "op": )" + (k % 2 ? R"({"add": 1})" : "7") + "}";
            edits += "]";
            mods.push_back(mod("r" + std::to_string(m), {batch(s, "r" + std::to_string(m), "alter", edits)}));
        }
        double best = 1e9;
        size_t refused = 1;
        for (int run = 0; run < 3; ++run) {
            const auto t0 = std::chrono::steady_clock::now();
            const Result r = compose(s, b, old, mods);
            best = std::min(best, ms_since(t0));
            refused = r.refused.size();
        }
        std::printf("registry-review2-test: 4,096 old changes + 50,000 registry edits composed in %.1f ms (best of 3, this build's flags)\n", best);
        std::string globs = "[";
        for (int i = 0; i < 1000; ++i) globs += std::string(i ? "," : "") + R"({"registry": "items", "select": {"id": "game:weapon_*"}, "field": "maxStack", "op": {"add": 1}})";
        globs += "]";
        const Batch g = batch(s, "a", "alter", globs);
        const auto t1 = std::chrono::steady_clock::now();
        compose(s, b, {}, {mod("a", {g})});
        const double glob_ms = ms_since(t1);
        std::printf("registry-review2-test: 1,000 globs matching nothing over 4,268 items in %.1f ms\n", glob_ms);
        CHECK(refused == 0, "the plan's load composes without refusals");
#ifdef KILN_BUDGET
        CHECK(best < 100, "the plan's load: under 100 ms at -O2 (P1-PLAN.md 5.3)");
        CHECK(glob_ms < 20, "a glob with a literal prefix looks at the entries with that prefix only");
#endif
    }

    std::printf("registry-review2-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
