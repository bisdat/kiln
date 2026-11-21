// SPDX-License-Identifier: MIT
// Host test of the edit IR's Advanced tier (owner decision D5): raw member paths in registry edits (registry/ir.cpp's reader,
// registry/compose.cpp's per-entry resolution), and the conflict lines the report needs (exclusive, certainty). The IR's
// published example (schema/examples/registry-ir.json, argv[1]) is read by the same reader, so the JSON Schema and the
// native reader agree on it.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../registry/compose.h"

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

static FieldSpec spec(const char* name, FieldType t, Applies a = Applies::Now) {
    FieldSpec f;
    f.name = name, f.type = t, f.applies = a;
    return f;
}

static Schema schema(bool raw_items = true) {
    Schema s;
    RegistrySpec items;
    items.name = "items", items.version = "1.0.0", items.raw = raw_items;
    FieldSpec ms = spec("maxStack", FieldType::Int, Applies::WorldLoad);
    ms.min = 1, ms.max = 65535;
    FieldSpec pw = spec("power", FieldType::Float);
    pw.min = 0, pw.max = 100000;
    FieldSpec perks = spec("perks", FieldType::List);
    perks.element = FieldType::Id, perks.max_items = 3;
    FieldSpec rarity = spec("rarity", FieldType::Enum);
    rarity.enum_values = {"Common", "Uncommon", "Rare", "Epic", "Legendary"};
    for (const FieldSpec& f : {ms, pw, perks, rarity}) items.fields[f.name] = f;
    s.registries["items"] = items;
    RegistrySpec bal;
    bal.name = "balance", bal.version = "1.0.0", bal.add = bal.remove = false;  // raw not allowed here
    FieldSpec cap = spec("levelCap", FieldType::Int, Applies::Restart);
    cap.min = 1, cap.max = 100;
    bal.fields[cap.name] = cap;
    s.registries["balance"] = bal;
    return s;
}

static const std::string kSword = "game:weapon_t1_sword", kAxe = "game:weapon_t1_axe", kLogs = "game:material_t1_wood_logs";

// The game's members as S5's model would resolve them: weapons have damageSetup (float), every item has maxStackSize (u16,
// a Kiln field's member), rarity (an enum), itemId (an identity, read-only) and a u8 tier; materials have no damageSetup.
static BaseModel base() {
    BaseModel b;
    auto item = [](const std::string& id, int64_t stack) {
        BaseModel::Entry e;
        e.id = id;
        e.fields["maxStack"] = Value::integer(stack);
        e.fields["power"] = Value::number(10);
        e.fields["perks"] = Value::of_list({});
        return e;
    };
    b.registries["items"] = {item(kSword, 1), item(kAxe, 1), item(kLogs, 10)};
    BaseModel::Entry bal;
    bal.id = "game:balance";
    bal.fields["levelCap"] = Value::integer(50);
    b.registries["balance"] = {bal};
    b.raw = [](const std::string& reg, const std::string& id, const std::string& key, std::string* why) -> std::optional<RawMember> {
        if (reg != "items") return *why = "no raw members in " + reg, std::nullopt;
        RawMember m;
        if (key == "damagesetup.dmgmod") {
            if (id.find("weapon") == std::string::npos) return *why = "keen::ItemInfo of " + id + " has no damageSetup on this build", std::nullopt;
            m.spec = spec("damageSetup.dmgMod", FieldType::Float);
            m.value = Value::number(1.0);
            return m;
        }
        if (key == "maxstacksize") {
            m.spec = spec("maxStackSize", FieldType::Int, Applies::WorldLoad);
            m.spec.min = 0, m.spec.max = 65535;
            m.value = Value::integer(id == kLogs ? 10 : 1);
            return m;
        }
        if (key == "tier") {
            m.spec = spec("tier", FieldType::Int);
            m.spec.min = 0, m.spec.max = 255;
            m.value = Value::integer(1);
            return m;
        }
        if (key == "itemid") {
            m.spec = spec("itemId", FieldType::Int);
            m.spec.read_only = true;
            m.value = Value::integer(42);
            return m;
        }
        if (key == "rarityoverride") {
            m.spec = spec("rarityOverride", FieldType::Enum);
            m.spec.enum_values = {"None", "Common", "Legendary"};
            m.value = Value::text("None");
            return m;
        }
        if (key == "damagesetup.power") {  // the member behind Kiln's power (converted through gear's formula)
            m.spec = spec("damageSetup.power", FieldType::Float);
            m.value = Value::number(3.0);
            return m;
        }
        return *why = "keen::ItemInfo has no member " + key + " on this build", std::nullopt;
    };
    b.raw_cover["items"]["maxstacksize"] = {"maxStack", false};
    b.raw_cover["items"]["damagesetup.power"] = {"power", true};
    return b;
}

static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits, std::vector<Refusal>* refused = nullptr) {
    Batch b;
    std::vector<Refusal> r;
    std::string err;
    const bool ok = parse_batch(std::string(R"({"ir": 1, "mod": ")") + mod + R"(", "stage": ")" + stage + R"(", "source": "content/t.json", "edits": )" + edits + "}", s, &b, &r, &err);
    if (!ok) std::printf("batch: %s\n", err.c_str());
    if (refused) *refused = r;
    else
        for (const Refusal& x : r) std::printf("unexpected refusal %s #%u: %s\n", x.mod.c_str(), x.n, x.reason.c_str());
    return b;
}

static ModInput mod(const std::string& id, std::vector<Batch> batches) {
    ModInput m;
    m.id = id;
    m.batches = std::move(batches);
    return m;
}

static Value result(const Result& r, const std::string& reg, const std::string& id, const std::string& field) {
    const auto it = r.fields.find({reg, id, field});
    return it == r.fields.end() ? Value{} : it->second.result;
}

static int lines(const Result& r, LineKind k) {
    int n = 0;
    for (const Line& l : r.lines) n += l.kind == k;
    return n;
}

static const Line* line(const Result& r, LineKind k) {
    for (const Line& l : r.lines)
        if (l.kind == k) return &l;
    return nullptr;
}

static bool refused_with(const Result& r, const std::string& mod, const char* text) {
    for (const Refusal& x : r.refused)
        if (x.mod == mod && x.reason.find(text) != std::string::npos) return true;
    return false;
}

static std::string alter_raw(const std::string& id, const std::string& raw, const std::string& extra = "") {
    return R"([{"registry": "items", "select": {"id": ")" + id + R"("}, "raw": )" + raw + extra + "}]";
}

int main(int argc, char** argv) {
    const Schema s = schema();
    const BaseModel b = base();
    const std::string dmg = raw_field("damagesetup.dmgmod");

    // ---- raw_key: one spelling per place ----
    {
        std::string k, err;
        CHECK(raw_key("DamageSetup.DmgMod", &k, &err) && k == "damagesetup.dmgmod", "raw_key: member names lower-case");
        CHECK(raw_key("a.B[03].c[0]", &k, &err) && k == "a.b[3].c[0]", "raw_key: indices as plain numbers");
        CHECK(raw_key("_x9.y_[12][4]", &k, &err) && k == "_x9.y_[12][4]", "raw_key: underscores, digits after the first letter, nested indices");
        for (const char* bad : {"", "a..b", ".a", "a.", "a[x]", "a.*", "a[*]", "1abc", "a[]", "a[1", "[0]", "a b", "a.b-c", "a[99999999999]"}) {
            err.clear();
            const bool ok = raw_key(bad, &k, &err);
            CHECK(!ok && !err.empty(), (std::string("raw_key refuses \"") + bad + "\"").c_str());
        }
    }

    // ---- the IR's checks for raw edits ----
    {
        std::vector<Refusal> r;
        Batch x = batch(s, "a", "alter", R"([
            {"registry": "items", "select": {"id": "game:weapon_*"}, "raw": {"damageSetup.dmgMod": 1.5, "Perks[02]": {"set": "game:perk_x"}}},
            {"registry": "balance", "select": {"id": "game:balance"}, "raw": {"levelCap": 60}},
            {"registry": "items", "select": {"id": "x"}, "field": "power", "op": 1, "raw": {"a": 1}},
            {"registry": "items", "select": {"id": "x"}, "raw": {}},
            {"registry": "items", "select": {"id": "x"}, "raw": [1]},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a..b": 1}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"DamageSetup.DmgMod": 1, "damageSetup.dmgMod": 2}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a": [1, 2]}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a": {"mul": "x"}}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a": {"min": 5, "max": 2}}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a": {"pow": 2}}},
            {"registry": "items", "raw": {"a": 1}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a": {"set": [1]}}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a": null}},
            {"registry": "items", "entry": "remove", "select": {"id": "x"}, "raw": {"a": 1}},
            {"registry": "items", "select": {"id": "x"}, "raw": {"a": {"mul": 2, "raiseOnly": true}}, "exclusive": true}
        ])", &r);
        CHECK(x.edits.size() == 2, "IR raw: two good edits kept");
        if (x.edits.size() == 2) {
            const Edit& e = x.edits[0];
            CHECK(e.raw.size() == 2 && e.field.empty(), "IR raw: both paths of the first edit kept, no Kiln field");
            const RawWrite* dm = nullptr;
            const RawWrite* pk = nullptr;
            for (const RawWrite& w : e.raw) (w.key == "damagesetup.dmgmod" ? dm : pk) = &w;
            CHECK(dm && dm->path == "damageSetup.dmgMod" && dm->op == 1.5, "IR raw: a path keeps its spelling and gets its key; the value is kept for the member's type");
            CHECK(pk && pk->key == "perks[2]" && pk->path == "Perks[02]", "IR raw: an index path");
            CHECK(x.edits[1].exclusive && x.edits[1].raw.size() == 1, "IR raw: exclusive with a relative raw op");
        }
        auto has = [&](uint32_t n, const char* text) {
            for (const Refusal& f : r)
                if (f.n == n && f.reason.find(text) != std::string::npos) return true;
            return false;
        };
        CHECK(r.size() == 14, "IR raw: fourteen edits refused, each on its own");
        CHECK(has(1, "not allowed"), "IR raw: a registry whose schema does not allow raw paths refuses them");
        CHECK(has(2, "one or the other"), "IR raw: raw together with field/op is refused");
        CHECK(has(3, "at least one"), "IR raw: an empty raw object is refused");
        CHECK(has(4, "\"raw\" is"), "IR raw: raw must be an object of paths");
        CHECK(has(5, "a..b"), "IR raw: a bad path is refused and named");
        CHECK(has(6, "one place"), "IR raw: two spellings of one place in one edit are refused");
        CHECK(has(7, "plain value"), "IR raw: a list value is refused (lists go through Kiln fields)");
        CHECK(has(8, "takes a number"), "IR raw: a relative op takes numbers");
        CHECK(has(9, "min must not be greater than max"), "IR raw: min > max is refused");
        CHECK(has(10, "unknown operation"), "IR raw: an unknown op is refused");
        CHECK(has(11, "select"), "IR raw: a raw edit names its entries");
        CHECK(has(12, "plain value"), "IR raw: set takes a plain value");
        CHECK(has(13, "plain value"), "IR raw: null is not a value");
        CHECK(has(14, "one or the other"), "IR raw: raw on an entry remove is refused");
        std::vector<Refusal> r2;
        batch(s, "a", "add", R"([{"registry": "items", "select": {"id": "x"}, "raw": {"a": 1}}])", &r2);
        CHECK(r2.size() == 1 && r2[0].reason.find("alter or final") != std::string::npos, "IR raw: stage add takes no raw edit");
        std::string many = "{";
        for (int i = 0; i < 33; ++i) many += (i ? ", " : "") + std::string("\"m") + std::to_string(i) + "\": 1";
        many += "}";
        std::vector<Refusal> r3;
        batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "x"}, "raw": )" + many + "}]", &r3);
        CHECK(r3.size() == 1 && r3[0].reason.find("32") != std::string::npos, "IR raw: at most 32 paths in one edit");
        std::vector<Refusal> r4;
        batch(schema(false), "a", "alter", R"([{"registry": "items", "select": {"id": "x"}, "raw": {"a": 1}}])", &r4);
        CHECK(r4.size() == 1 && r4[0].reason.find("Advanced") != std::string::npos, "IR raw: the refusal names the Advanced tier");
    }

    // ---- composition: a raw path resolved per entry, typed by the member, composed like a field ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.dmgMod": 1.5})"))})});
        CHECK(r.refused.empty() && result(r, "items", kSword, dmg).d == 1.5, "a raw set on a member that resolves");
        const auto it = r.fields.find({"items", kSword, dmg});
        CHECK(it != r.fields.end() && it->second.raw == "damageSetup.dmgMod" && it->second.raw_spec && it->second.raw_spec->type == FieldType::Float &&
                  it->second.base.d == 1.0 && it->second.chain.size() == 1 && it->second.chain[0].raw == "damageSetup.dmgMod",
              "the raw chain keeps the path as written, the member's spec, the base and the row");
        const Result st = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw("game:weapon_*", R"({"damageSetup.dmgMod": {"mul": 2}})"))}),
                                             mod("b", {batch(s, "b", "alter", alter_raw(kSword, R"({"DamageSetup.DmgMod": {"mul": 3}})"))})});
        CHECK(result(st, "items", kSword, dmg).d == 6.0 && result(st, "items", kAxe, dmg).d == 2.0 && lines(st, LineKind::Composed) == 1,
              "relative raw edits stack across mods, whatever the spelling of the path");
        const Result glob = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw("*", R"({"damageSetup.dmgMod": 2})"))})});
        CHECK(glob.fields.empty() && glob.refused.size() == 1 && refused_with(glob, "a", "does not resolve") && refused_with(glob, "a", "damageSetup.dmgMod") &&
                  refused_with(glob, "a", kLogs.c_str()),
              "one entry whose member does not resolve refuses the whole edit, naming the entry and the path (never a blind write)");
        BaseModel none = b;
        none.raw = nullptr;
        const Result off = compose(s, none, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.dmgMod": 2})"))})});
        CHECK(off.fields.empty() && off.refused.size() == 1, "without the model's raw lookup no raw path resolves");
        const Result types = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([
            {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"tier": 1.5}},
            {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"tier": 300}},
            {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"tier": {"mul": 1000}}},
            {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"itemId": 7}},
            {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"rarityOverride": "legendary"}},
            {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"rarityOverride": {"mul": 2}}},
            {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"tier": 3, "nope.nothing": 1}}
        ])")})});
        CHECK(refused_with(types, "a", "whole number"), "a raw value is checked against the member's type");
        CHECK(refused_with(types, "a", "outside"), "a raw value is checked against the member's range (a u8)");
        CHECK(refused_with(types, "a", "read-only"), "a read-only member (an identity) is refused");
        CHECK(refused_with(types, "a", "works on numbers"), "a relative op on an enum member is refused");
        CHECK(result(types, "items", kSword, raw_field("rarityoverride")).t == "Legendary", "an enum member takes a name in any case, kept in the game's spelling");
        CHECK(result(types, "items", kSword, raw_field("tier")).kind == Value::None, "an edit with one path that does not resolve writes none of its paths");
        CHECK(types.refused.size() == 6, "six of the seven raw edits refused");
    }

    // ---- a raw path that is a Kiln field's member: one place, one chain ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kLogs, R"({"maxStackSize": 100})"))}),
                                            mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 2}}])")})});
        CHECK(result(r, "items", kLogs, "maxStack").i == 200 && r.fields.count({"items", kLogs, raw_field("maxstacksize")}) == 0,
              "a raw edit of a covered member joins the Kiln field's chain");
        const auto& chain = r.fields.at({"items", kLogs, "maxStack"}).chain;
        CHECK(chain.size() == 2 && chain[0].raw == "maxStackSize" && chain[1].raw.empty() && lines(r, LineKind::ComposedOn) == 1,
              "the chain shows the raw row, and the field edit composes on it (reported)");
        const Result rev = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 50}])")}),
                                              mod("b", {batch(s, "b", "alter", alter_raw(kLogs, R"({"maxStackSize": 70})"))})});
        CHECK(result(rev, "items", kLogs, "maxStack").i == 70 && lines(rev, LineKind::Overridden) == 1, "a raw edit after a field edit on one place overrides it, reported");
        const Result range = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kLogs, R"({"maxStackSize": 0})"))})});
        CHECK(range.refused.size() == 1 && refused_with(range, "a", "outside"), "a covered raw edit is checked against the Kiln field's range");
    }

    // ---- a raw path behind a field Kiln converts (one place, two scales; work-bb's D1 ruling): a value wins over the other
    // scale's writers, reported; a relative change (no conversion known here) is refused ----
    {
        const std::string pw_edit = R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "power", "op": 50}])";
        const FieldRef pw{"items", kSword, "power"}, dp{"items", kSword, raw_field("damagesetup.power")};
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", pw_edit)}),
                                            mod("b", {batch(s, "b", "alter", alter_raw(kSword, R"({"damageSetup.power": 2})"))})});
        const Line* ov = line(r, LineKind::Overridden);
        CHECK(r.refused.empty() && result(r, "items", kSword, raw_field("damagesetup.power")).d == 2 && !r.fields.count(pw) && ov && ov->winner == "b" && ov->loser == "a",
              "a raw value over a field edit of the converted place wins; the field's chain goes; reported");
        const Result rel = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", pw_edit)}),
                                              mod("b", {batch(s, "b", "alter", alter_raw(kSword, R"({"damageSetup.power": {"mul": 2}})"))})});
        CHECK(result(rel, "items", kSword, "power").d == 50 && rel.refused.size() == 1 && refused_with(rel, "b", "no conversion") && lines(rel, LineKind::Exclusive) == 1,
              "a relative raw change over the converted field is refused and reported");
        const Result rev = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.power": 2})"))}),
                                              mod("b", {batch(s, "b", "alter", pw_edit)})});
        CHECK(result(rev, "items", kSword, "power").d == 50 && !rev.fields.count(dp) && rev.refused.empty(), "...and the other way round: the later field value wins");
        const Result own = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", pw_edit), batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.power": 2})"))})});
        CHECK(own.refused.empty() && result(own, "items", kSword, raw_field("damagesetup.power")).d == 2 && !own.fields.count(pw), "one mod's later value wins over its own");
        OldWrite pack;
        pack.kind = WriterKind::OldPack, pack.mod = "old", pack.who = "old: content/a.json", pack.field = pw;
        pack.op.set = true, pack.op.value = Value::number(40);
        const Result old = compose(s, b, {pack}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.power": 2})"))})});
        CHECK(old.refused.empty() && result(old, "items", kSword, raw_field("damagesetup.power")).d == 2 && !old.fields.count(pw) && line(old, LineKind::Overridden),
              "a raw value over an old path's write of the converted field wins, reported (D1)");
        const Result oldrel = compose(s, b, {pack}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.power": {"add": 1}})"))})});
        CHECK(oldrel.refused.size() == 1 && result(oldrel, "items", kSword, "power").d == 40, "a relative raw change over an old write of the converted field is refused; the old write stays");
        const Result alone = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.power": 2})"))})});
        CHECK(alone.refused.empty() && result(alone, "items", kSword, raw_field("damagesetup.power")).d == 2,
              "with no writer of the field there, the raw edit applies (the Advanced tier works while the names hold)");
    }

    // ---- the edit rules hold for raw paths: exclusive, yield, final, old paths below ----
    {
        const Result ex = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.dmgMod": 2})", R"(, "exclusive": true)"))}),
                                             mod("b", {batch(s, "b", "alter", alter_raw(kSword, R"({"damageSetup.dmgMod": 3})"))})});
        CHECK(result(ex, "items", kSword, dmg).d == 2 && ex.refused.size() == 1 && refused_with(ex, "b", "exclusive"), "exclusive on a raw path");
        const Line* l = line(ex, LineKind::Exclusive);
        CHECK(l && l->winner == "a" && l->loser == "b" && l->certainty == Certainty::Certain, "an exclusive refusal is a conflict line too (winner, loser, certain)");
        const Result y = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.dmgMod": 2})"))}),
                                            mod("b", {batch(s, "b", "alter", alter_raw("game:weapon_*", R"({"damageSetup.dmgMod": 5})", R"(, "yield": true)"))})});
        CHECK(result(y, "items", kSword, dmg).d == 2 && result(y, "items", kAxe, dmg).d == 5 && lines(y, LineKind::Yielded) == 1, "yield on a raw path");
        const Result fin = compose(s, b, {}, {mod("a", {batch(s, "a", "final", alter_raw(kSword, R"({"damageSetup.dmgMod": {"add": 1}})"))}),
                                              mod("b", {batch(s, "b", "final", alter_raw(kSword, R"({"damageSetup.dmgMod": {"mul": 4}})"))})});
        CHECK(result(fin, "items", kSword, dmg).d == 8 && fin.refused.empty(), "raw edits in final: composed after every final, in load order");
        OldWrite old;
        old.kind = WriterKind::OldPack, old.mod = "old", old.who = "old: content/a.json", old.field = {"items", kSword, dmg};
        old.op.set = true, old.op.value = Value::number(3);
        const Result on = compose(s, b, {old}, {mod("a", {batch(s, "a", "alter", alter_raw(kSword, R"({"damageSetup.dmgMod": {"mul": 2}})"))})});
        CHECK(result(on, "items", kSword, dmg).d == 6 && on.refused.empty() && lines(on, LineKind::ComposedOn) == 1,
              "an old path's write of a raw member lies below; a relative raw edit composes on it");
        const Result add = compose(s, b, {}, {mod("a", {batch(s, "a", "add", R"([{"registry": "items", "entry": "add", "id": "a:blade", "like": "game:weapon_t1_sword"}])"),
                                                        batch(s, "a", "alter", alter_raw("a:blade", R"({"damageSetup.dmgMod": 4})"))})});
        CHECK(add.refused.empty() && result(add, "items", "a:blade", dmg).d == 4, "an added entry resolves raw members through its like entry");
        const Result bare = compose(s, b, {}, {mod("a", {batch(s, "a", "add", R"([{"registry": "items", "entry": "add", "id": "a:thing"}])"),
                                                         batch(s, "a", "alter", alter_raw("a:thing", R"({"damageSetup.dmgMod": 4})"))})});
        CHECK(bare.refused.size() == 1 && refused_with(bare, "a", "like"), "an added entry without like has no game member to write");
    }

    // ---- certainty: a conflict with an archive-writer package is named, not certain ----
    {
        OldWrite sf;
        sf.kind = WriterKind::ArchiveWriter, sf.who = "shroudforge:betterstacks", sf.field = {"items", kLogs, "maxStack"};
        sf.op.set = true, sf.op.value = Value::integer(20);
        const Result r = compose(s, b, {sf}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 30}])")})});
        const Line* l = line(r, LineKind::Overridden);
        CHECK(l && l->certainty == Certainty::Named && l->loser == "shroudforge:betterstacks", "an override of an archive-writer value is named");
        CHECK(std::string(certainty_name(Certainty::Certain)) == "certain" && std::string(certainty_name(Certainty::Named)) == "named" &&
                  std::string(certainty_name(Certainty::BestEffort)) == "bestEffort",
              "certainty names (registry.json)");
    }

    // ---- the published IR example reads with the native reader, every edit taken ----
    if (argc > 1) {
        std::ifstream in(argv[1]);
        std::stringstream text;
        text << in.rdbuf();
        Schema ex = schema();
        RegistrySpec recipes;
        recipes.name = "recipes", recipes.version = "1.0.0";
        FieldSpec dur = spec("duration", FieldType::Float);
        dur.min = 0, dur.max = 3600;
        recipes.fields[dur.name] = dur;
        ex.registries["recipes"] = recipes;
        Batch x;
        std::vector<Refusal> r;
        std::string err;
        const bool ok = parse_batch(text.str(), ex, &x, &r, &err);
        CHECK(ok && r.empty() && x.edits.size() >= 6, (std::string("schema/examples/registry-ir.json reads with every edit taken: ") + err +
                                                       (r.empty() ? "" : " / " + r[0].reason)).c_str());
    } else {
        CHECK(false, "argv[1]: the path of schema/examples/registry-ir.json");
    }

    std::printf("registry-raw-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
