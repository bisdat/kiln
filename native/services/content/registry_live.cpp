// SPDX-License-Identifier: MIT
// kiln.content: the registry's live back-end (registry P1 slice S9). What it does and why: registry_live.h.
#include "registry_live.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "registry/formulas.h"

namespace kc {

uint32_t (*g_registry_type_size)(const char* qname) = &type_size;

namespace {

using registry::ChainRow;
using registry::FieldSpec;
using registry::FieldType;
using registry::Value;
namespace rmap = registry::map;

struct LiveState {
    std::shared_ptr<RegistryLive> in;
    std::map<std::string, uint64_t> hashes;  // rel -> section hash, last application
    bool applied = false;                     // a first application happened since the install
    std::unordered_map<std::string, std::string> kiln_of;  // "<registry>|<32-bit id>" -> Kiln id (from nums)
    std::unordered_map<std::string, std::string> kiln_by_guid;  // "<registry>|<guid, lower case>" -> Kiln id (from ids)
    std::vector<registry::Refusal> refused;        // this application's live refusals
    std::set<std::string> refused_keys;
    struct Pending {
        registry::FieldRef field;
        std::string guid;
    };
    std::vector<Pending> pending_tables;  // drop-table fields whose table the world has not loaded yet
    registry::Result reported;
    bool have_report = false;
    uint32_t ordinal = 0;
    KilnOwner owner = 0;
};
LiveState g;  // under store_mutex

uint32_t kind_of(const std::string& registry) {
    if (registry == "items") return KILN_CONTENT_ITEMS;
    if (registry == "recipes") return KILN_CONTENT_RECIPES;
    if (registry == "loot") return KILN_CONTENT_LOOT;
    if (registry == "drops") return KILN_CONTENT_DROPS;
    return 0;
}

std::string num_key(const std::string& registry, uint32_t num) { return registry + "|" + std::to_string(num); }

std::vector<const ChainRow*> registry_rows(const registry::FieldOutcome& fo) {
    std::vector<const ChainRow*> out;
    for (const ChainRow& r : fo.chain)
        if (r.kind == registry::WriterKind::Registry) out.push_back(&r);
    return out;
}

// One refusal per (field, reason): the edit that could not be written live.
void refuse_rows(const registry::FieldRef& fr, const std::string& mod, const std::string& source, registry::Stage stage, uint32_t n, const std::string& why) {
    const std::string key = fr.text() + "|" + why;
    if (!g.refused_keys.insert(key).second) return;
    registry::Refusal r;
    r.mod = mod, r.source = source, r.stage = stage, r.n = n;
    const bool said = why.size() >= 11 && why.compare(why.size() - 11, 11, "not applied") == 0;
    r.reason = fr.text() + ": " + why + (said ? "" : "; this edit was not applied");
    g.refused.push_back(std::move(r));
}
void refuse(const registry::FieldRef& fr, const std::vector<const ChainRow*>& rows, const std::string& why) {
    if (rows.empty()) return refuse_rows(fr, "", "", registry::Stage::Alter, 0, why);
    refuse_rows(fr, rows.back()->mod, rows.back()->source, rows.back()->stage, rows.back()->n, why);
}

// The game entry of a Kiln id in this world.
const Entry* find_entry(const std::string& registry, const std::string& id, std::string* guid_out) {
    const uint32_t kind = kind_of(registry);
    if (!kind) return nullptr;
    const Registry& r = reg(kind);
    const std::string key = registry + "|" + id;
    std::string guid;
    if (auto it = g.in->ids.find(key); it != g.in->ids.end()) guid = it->second;
    const bool added = std::find(g.in->result.added.begin(), g.in->result.added.end(), registry::EntryRef{registry, id}) != g.in->result.added.end();
    if (guid.empty() && added && g.in->added_guid) guid = g.in->added_guid(registry, id);
    if (guid_out) *guid_out = guid;
    if (kind == KILN_CONTENT_DROPS) return guid.empty() ? nullptr : r.lookup(guid);
    if (auto it = g.in->nums.find(key); it != g.in->nums.end()) return r.get(it->second);
    if (guid.empty()) return nullptr;
    for (const Entry& e : r.entries)
        if (lower(guid_text(e.guid)) == lower(guid)) return &e;
    return nullptr;
}

// ---- values: Kiln <-> the game's ----
// The game's enum member name for a Kiln name (the row's values), and back.
std::string game_name(const rmap::Row* row, const std::string& kiln) {
    if (row)
        if (auto it = row->values.find(kiln); it != row->values.end()) return it->second;
    return kiln;
}
std::string kiln_name(const rmap::Row* row, const std::string& game) {
    if (row)
        for (auto& [k, v] : row->values)
            if (v == game || lower(v) == lower(game)) return k;
    return game;
}

Value to_kiln(const Field& f, const rmap::Row* row, const FieldSpec& spec, const uint8_t* bytes) {
    const KilnContentValue kv = decode(f, bytes);
    switch (spec.type) {
        case FieldType::Int:
            return Value::integer(kv.type == KILN_CV_FLOAT || kv.type == KILN_CV_TIME ? std::llround(as_double(kv)) : kv.i);
        case FieldType::Float: return Value::number(as_double(kv));
        case FieldType::Bool: return Value::boolean(kv.i != 0);
        case FieldType::Enum:
            if (f.en)
                for (auto& [n, x] : f.en->values)
                    if (x == kv.i) return Value::text(kiln_name(row, n));
            return Value::text(std::to_string(kv.i));
        default: return Value{};
    }
}

bool to_game(const Field& f, const rmap::Row* row, const FieldSpec& spec, const Value& v, uint8_t* out, std::string* why) {
    KilnContentValue kv{};
    switch (spec.type) {
        case FieldType::Int:
            kv.type = KILN_CV_INT, kv.i = v.kind == Value::Int ? v.i : std::llround(v.as_double()), kv.f = static_cast<double>(kv.i);
            break;
        case FieldType::Float:
            kv.type = f.vt == KILN_CV_TIME ? KILN_CV_TIME : KILN_CV_FLOAT, kv.f = v.as_double();
            if (kv.type == KILN_CV_TIME && kv.f == 0) kv.type = KILN_CV_INT, kv.i = 0;  // 0 s (encode reads a TIME of f 0 from i)
            break;
        case FieldType::Bool: kv.type = KILN_CV_BOOL, kv.i = v.kind == Value::Bool ? v.b : v.as_double() != 0; break;
        case FieldType::Enum: {
            const std::string name = game_name(row, v.t);
            bool found = false;
            if (f.en)
                for (auto& [n, x] : f.en->values)
                    if (!found && (n == name || lower(n) == lower(name))) kv.type = KILN_CV_INT, kv.i = x, found = true;
            if (!found) return *why = "\"" + v.t + "\" is not a value of " + f.path + " on this game build", false;
            break;
        }
        default: return *why = "this kind of field is not written live", false;
    }
    return encode(f, kv, out, why);
}

bool in_range(const FieldSpec& spec, const Value& v, std::string* why) {
    if (!v.numeric()) return true;
    const double d = v.as_double();
    if ((spec.min && d < *spec.min) || (spec.max && d > *spec.max) || !std::isfinite(d))
        return *why = "the result " + v.text_of() + " over the value below is outside the field's range", false;
    return true;
}

gear::Constants constants() {
    std::map<std::string, double> balance;
    for (const auto& [fr, fo] : g.in->result.fields)
        if (fr.registry == "balance" && fr.field.rfind("gear.", 0) == 0 && fo.result.numeric()) balance[fr.field] = fo.result.as_double();
    return registry::formulas::constants_of(balance);
}

// One field's binding: what every evaluated layer of it shares.
struct Binding {
    registry::FieldRef ref;
    std::vector<ChainRow> rows;  // the registry rows, in chain order
    FieldSpec spec;
    std::optional<rmap::Row> row;  // empty: a raw member
    uint32_t kind = 0;
    Entry entry;
};

bool fail(const std::shared_ptr<Binding>& b, const std::string& why) {
    const ChainRow& r = b->rows.back();
    refuse_rows(b->ref, r.mod, r.source, r.stage, r.n, why);
    return false;
}

std::string source_of(const Binding& b) {
    const ChainRow& r = b.rows.back();
    return (r.mod.empty() ? r.who : r.mod) + ": " + r.source + " (registry " + b.ref.registry + "[" + b.ref.id + "]." + b.ref.field + ")";
}

bool put(const std::shared_ptr<Binding>& b, int32_t field_index, int32_t sub, uintptr_t addr, uint8_t n, LayerEval ev) {
    LayerModScope scope(b->rows.back().mod);
    return store_put_eval(registry_owner(), registry_seq(++g.ordinal), b->kind, b->entry.id, field_index, sub, addr, n, std::move(ev), source_of(*b));
}

bool put_at(const std::shared_ptr<Binding>& b, uint32_t kind, uint32_t id, int32_t field_index, int32_t sub, uintptr_t addr, uint8_t n, LayerEval ev) {
    LayerModScope scope(b->rows.back().mod);
    return store_put_eval(registry_owner(), registry_seq(++g.ordinal), kind, id, field_index, sub, addr, n, std::move(ev), source_of(*b));
}

int32_t index_of(uint32_t kind, const Field* f) { return static_cast<int32_t>(f - reg(kind).schema.fields.data()); }

// ---- scalars: int, float, bool, time, enum; raw members ----
void bind_scalar(const std::shared_ptr<Binding>& b, const Field* f) {
    const Field fc = *f;
    LayerEval ev = [b, fc](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
        const rmap::Row* row = b->row ? &*b->row : nullptr;
        const Value v = registry::evaluate(b->rows, to_kiln(fc, row, b->spec, below), b->spec);
        if (!in_range(b->spec, v, why) || !to_game(fc, row, b->spec, v, out, why)) return fail(b, *why);
        return true;
    };
    if (!put(b, index_of(b->kind, f), -1, b->entry.addr + f->offset, f->bytes, std::move(ev))) fail(b, "the field is not readable");
}

// ---- formulas: power, armor ----
void bind_formula(const std::shared_ptr<Binding>& b, const std::vector<const Field*>& inputs) {
    std::vector<Field> fs;
    for (const Field* f : inputs) fs.push_back(*f);
    const uintptr_t base = b->entry.addr;
    LayerEval ev = [b, fs, base](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
        std::vector<double> in;
        in.push_back(as_double(decode(fs[0], below)));
        for (size_t k = 1; k < fs.size(); ++k) {
            uint8_t x[kSlotBytes] = {};
            if (!store_effective(base + fs[k].offset, fs[k].bytes, x)) return fail(b, *why = fs[k].path + " is not readable");
            in.push_back(as_double(decode(fs[k], x)));
        }
        const gear::Constants c = constants();
        double kiln = 0, game = 0;
        if (!registry::formulas::read(b->row->formula, in, c, &kiln)) return fail(b, *why = "the formula " + b->row->formula + " has no value for this entry");
        const Value v = registry::evaluate(b->rows, Value::integer(std::llround(kiln)), b->spec);
        if (!in_range(b->spec, v, why)) return fail(b, *why);
        if (!registry::formulas::write(b->row->formula, v.as_double(), in, c, &game))
            return fail(b, *why = "the formula " + b->row->formula + " gives no game value for " + v.text_of());
        KilnContentValue kv{};
        kv.type = KILN_CV_FLOAT, kv.f = game;
        if (!encode(fs[0], kv, out, why)) return fail(b, *why);
        return true;
    };
    if (!put(b, index_of(b->kind, inputs[0]), -1, base + inputs[0]->offset, inputs[0]->bytes, std::move(ev))) fail(b, "the field is not readable");
}

// ---- recipe lists: inputs, outputs ----
Value make_struct(std::vector<std::pair<std::string, Value>> kv) {
    std::sort(kv.begin(), kv.end(), [](auto& a, auto& b) { return a.first < b.first; });
    Value x;
    x.kind = Value::Struct;
    for (auto& [k, v] : kv) x.keys.push_back(k), x.list.push_back(std::move(v));
    return x;
}
// A game id as a Kiln id (an id the stage's table does not have is kept as "#<id>", so it goes back unchanged).
Value id_value(const std::string& registry, uint32_t num) {
    auto it = g.kiln_of.find(num_key(registry, num));
    return Value::text(it != g.kiln_of.end() ? it->second : "#" + std::to_string(num));
}
bool id_num(const std::string& registry, const Value& v, uint32_t* num) {
    if (v.kind != Value::Text || v.t.empty()) return false;
    if (v.t[0] == '#') return (*num = static_cast<uint32_t>(std::strtoul(v.t.c_str() + 1, nullptr, 10))) != 0;
    if (auto it = g.in->nums.find(registry + "|" + v.t); it != g.in->nums.end()) return *num = it->second, true;
    const Entry* e = find_entry(registry, v.t, nullptr);  // an entry a mod added: the id kiln.content gave its GUID
    if (!e) return false;
    *num = e->id;
    return true;
}

bool read_list(uintptr_t header, const uint8_t* hb, uint32_t which, std::vector<KilnContentStack>* out) {
    int32_t off = 0;
    uint32_t n = 0;
    std::memcpy(&off, hb, 4);
    std::memcpy(&n, hb + 4, 4);
    if (n > 256) return false;
    out->clear();
    const uint32_t stride = which == KILN_CONTENT_INPUTS ? RL.in_stride : RL.out_stride;
    for (uint32_t q = 0; q < n; ++q) {
        const uintptr_t p = header + static_cast<intptr_t>(off) + static_cast<uintptr_t>(q) * stride;
        KilnContentStack x{};
        if (which == KILN_CONTENT_INPUTS) {
            uint32_t ccount = 0;
            if (!rdv(p + RL.in_item, x.item) || !rdv(p + RL.in_count, x.count) || !rdv(p + RL.in_cat, x.category) || !rdv(p + RL.in_cat_count, ccount))
                return false;
            if (!x.item) x.count = ccount;
        } else if (!rdv(p + RL.out_item, x.item) || !rdv(p + RL.out_count, x.count)) {
            return false;
        }
        out->push_back(x);
    }
    return true;
}

Value list_value(const std::vector<KilnContentStack>& st, bool inputs) {
    std::vector<Value> l;
    for (const KilnContentStack& s : st) {
        if (s.item || !inputs) l.push_back(make_struct({{"count", Value::integer(s.count)}, {"item", id_value("items", s.item)}}));
        else l.push_back(make_struct({{"category", id_value("inputCategories", s.category)}, {"count", Value::integer(s.count)}}));
    }
    return Value::of_list(std::move(l));
}

bool stacks_of(const Value& v, bool inputs, std::vector<KilnContentStack>* out, std::string* why) {
    if (v.kind != Value::List) return *why = "no list", false;
    out->clear();
    for (const Value& e : v.list) {
        KilnContentStack s{};
        s.chance = 1;
        const Value* item = e.leaf("item");
        const Value* cat = e.leaf("category");
        const Value* count = e.leaf("count");
        s.count = s.count_max = count && count->numeric() ? static_cast<uint32_t>(std::llround(count->as_double())) : 1;
        if (item && item->kind == Value::Text) {
            if (!id_num("items", *item, &s.item)) return *why = "the item " + item->t + " is not in this world", false;
        } else if (inputs && cat && cat->kind == Value::Text) {
            if (!id_num("inputCategories", *cat, &s.category)) return *why = "the input category " + cat->t + " is not known on this game build", false;
        } else {
            return *why = inputs ? "an input needs an item or a category" : "an output needs an item", false;
        }
        out->push_back(s);
    }
    return true;
}

bool same_stacks(const std::vector<KilnContentStack>& x, const std::vector<KilnContentStack>& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i].item != y[i].item || x[i].category != y[i].category || x[i].count != y[i].count) return false;
    return true;
}

void bind_recipe_list(const std::shared_ptr<Binding>& b, uint32_t which) {
    struct Cache {
        std::vector<KilnContentStack> stacks;
        uint8_t hb[8] = {};
        bool have = false;
    };
    auto cache = std::make_shared<Cache>();
    const uintptr_t header = b->entry.addr + (which == KILN_CONTENT_INPUTS ? RL.input : RL.output);
    LayerEval ev = [b, cache, header, which](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
        std::vector<KilnContentStack> now;
        if (!read_list(header, below, which, &now)) return fail(b, *why = "the list below is not readable");
        const bool inputs = which == KILN_CONTENT_INPUTS;
        const Value v = registry::evaluate(b->rows, list_value(now, inputs), b->spec);
        std::vector<KilnContentStack> want;
        if (!stacks_of(v, inputs, &want, why)) return fail(b, *why);
        if (same_stacks(want, now)) {  // nothing changes over this value: the value below stays
            std::memcpy(out, below, 8);
            return true;
        }
        if (cache->have && same_stacks(want, cache->stacks)) {  // the list of last time: the same memory (near memory is never freed)
            std::memcpy(out, cache->hb, 8);
            return true;
        }
        if (recipe_list_header(b->entry, which, want, out, why) != KILN_OK) return fail(b, *why);
        cache->stacks = want, cache->have = true;
        std::memcpy(cache->hb, out, 8);
        return true;
    };
    if (!put(b, -1, static_cast<int32_t>(which), header, 8, std::move(ev))) fail(b, "the recipe is not readable");
}

// ---- drop tables: the leaves of each stack (entries[i].count.min, ...); a table keeps its stacks ----
struct DropLeaf {
    const char* leaf;   // the element's leaf name
    const char* field;  // kiln.content's drops field
};
constexpr DropLeaf kDropLeaves[] = {{"chance", "spawnProbability"}, {"count.max", "countMax"}, {"count.min", "countMin"}, {"weight", "weight"}};

void bind_drops(const std::shared_ptr<Binding>& b) {
    const Schema& sc = reg(KILN_CONTENT_DROPS).schema;
    std::vector<DropStackRef> refs;
    walk_drop_table(b->entry.addr, &refs);
    std::vector<const Field*> fields;
    for (const DropLeaf& l : kDropLeaves) {
        const Field* f = sc.find(l.field);
        if (!f) return (void)fail(b, std::string("the drop-table member ") + l.field + " is missing on this game build");
        fields.push_back(f);
    }
    std::vector<Field> fs;
    for (const Field* f : fields) fs.push_back(*f);
    // the whole list's value, the slot being evaluated taken from its own value below
    auto list_at = [refs, fs](size_t mine_stack, size_t mine_leaf, const uint8_t* below) {
        std::vector<Value> l;
        for (size_t j = 0; j < refs.size(); ++j) {
            uint32_t item = 0;
            rdv(refs[j].addr + DL.s_item, item);
            std::vector<std::pair<std::string, Value>> kv{{"item", id_value("items", item)}};
            for (size_t k = 0; k < fs.size(); ++k) {
                uint8_t x[kSlotBytes] = {};
                if (j == mine_stack && k == mine_leaf) std::memcpy(x, below, fs[k].bytes);
                else store_below_registry(refs[j].addr + fs[k].offset, fs[k].bytes, x);  // every leaf over the same list below
                const KilnContentValue cv = decode(fs[k], x);
                kv.emplace_back(kDropLeaves[k].leaf, fs[k].vt == KILN_CV_FLOAT ? Value::number(cv.f) : Value::integer(cv.i));
            }
            l.push_back(make_struct(std::move(kv)));
        }
        return Value::of_list(std::move(l));
    };
    const registry::RegistrySpec* drops = g.in->schema->registry("drops");
    for (size_t i = 0; i < refs.size(); ++i)
        for (size_t k = 0; k < fs.size(); ++k) {
            const FieldSpec* leaf_spec = drops ? drops->field(std::string("entries[].") + kDropLeaves[k].leaf) : nullptr;
            const FieldSpec lspec = leaf_spec ? *leaf_spec : FieldSpec{};
            const Field fk = fs[k];
            LayerEval ev = [b, list_at, i, k, fk, lspec](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
                const Value before = list_at(i, k, below);
                const Value v = registry::evaluate(b->rows, before, b->spec);
                if (v.kind != Value::List || v.list.size() != before.list.size())
                    return fail(b, *why = "a drop table keeps its stacks (only their counts, chances and weights change)");
                const Value* x = v.list[i].leaf(kDropLeaves[k].leaf);
                if (!x) return fail(b, *why = std::string("no ") + kDropLeaves[k].leaf);
                if (!in_range(lspec, *x, why)) return fail(b, *why);
                KilnContentValue kv{};
                if (fk.vt == KILN_CV_FLOAT) kv.type = KILN_CV_FLOAT, kv.f = x->as_double();
                else kv.type = KILN_CV_INT, kv.i = std::llround(x->as_double());
                if (!encode(fk, kv, out, why)) return fail(b, *why);
                return true;
            };
            put(b, index_of(KILN_CONTENT_DROPS, fields[k]), static_cast<int32_t>(i), refs[i].addr + fields[k]->offset, fields[k]->bytes, std::move(ev));
        }
}

// ---- perks (refList): five slots of a reference and an id, kept in step ----
bool zero_bytes(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}
Value perk_value(const uint8_t bytes[16]) {
    const std::string guid = lower(guid_text(bytes));
    auto it = g.kiln_by_guid.find("perks|" + guid);
    return Value::text(it != g.kiln_by_guid.end() ? it->second : "#guid:" + guid);
}
bool perk_guid(const Value& v, uint8_t out[16]) {
    if (v.kind != Value::Text) return false;
    if (v.t.rfind("#guid:", 0) == 0) return guid_parse(v.t.substr(6), out);
    auto it = g.in->ids.find("perks|" + v.t);
    return it != g.in->ids.end() && guid_parse(it->second, out);
}

void bind_perks(const std::shared_ptr<Binding>& b) {
    const Schema& sc = reg(KILN_CONTENT_ITEMS).schema;
    std::vector<Field> refs, ids;
    for (int i = 0; i < 5; ++i) {
        const Field* r = sc.find("perkReferences[" + std::to_string(i) + "]");
        const Field* d = sc.find("perkIds[" + std::to_string(i) + "]");
        if (!r || !d || r->bytes != 16 || d->bytes != 4) return (void)fail(b, "this game build's items have no perkReferences / perkIds");
        refs.push_back(*r), ids.push_back(*d);
    }
    const uintptr_t base = b->entry.addr;
    // the perks below the registry, slot i's reference given as below when it is the slot evaluated
    auto result = [b, refs, ids, base](int ref_slot, const uint8_t* below, std::vector<Value>* out, std::string* why) {
        std::vector<Value> l;
        for (int i = 0; i < 5; ++i) {
            uint8_t gb[16] = {};
            if (i == ref_slot) std::memcpy(gb, below, 16);
            else store_below_registry(base + refs[i].offset, 16, gb);
            if (!zero_bytes(gb, 16)) l.push_back(perk_value(gb));
        }
        const Value v = registry::evaluate(b->rows, Value::of_list(std::move(l)), b->spec);
        if (v.kind != Value::List || v.list.size() > 5) return *why = "an item has 5 perk slots", false;
        *out = v.list;
        return true;
    };
    for (int i = 0; i < 5; ++i) {
        LayerEval ref_ev = [b, result, i](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
            std::vector<Value> l;
            if (!result(i, below, &l, why)) return fail(b, *why);
            uint8_t gb[16] = {};
            if (static_cast<size_t>(i) < l.size() && !perk_guid(l[i], gb)) return fail(b, *why = "the perk " + l[i].t + " is not known on this game build");
            std::memcpy(out, gb, 16);
            return true;
        };
        const Field rf = refs[i];
        LayerEval id_ev = [b, result, i, rf, base](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
            uint8_t own[16] = {};
            store_below_registry(base + rf.offset, 16, own);
            std::vector<Value> l;
            if (!result(i, own, &l, why)) return fail(b, *why);
            uint32_t id = 0;
            if (static_cast<size_t>(i) < l.size()) {
                uint8_t gb[16] = {};
                if (!perk_guid(l[i], gb)) return fail(b, *why = "the perk " + l[i].t + " is not known on this game build");
                if (std::memcmp(gb, own, 16) == 0) std::memcpy(&id, below, 4);  // the same perk as below: its id as it is
                else if (auto it = g.in->nums.find("perks|" + l[i].t); it != g.in->nums.end() && it->second) id = it->second;
                else id = fnv1a(gb, 16);
            }
            std::memcpy(out, &id, 4);
            return true;
        };
        put(b, index_of(KILN_CONTENT_ITEMS, sc.find(refs[i].path)), -1, base + refs[i].offset, 16, std::move(ref_ev));
        put(b, index_of(KILN_CONTENT_ITEMS, sc.find(ids[i].path)), -1, base + ids[i].offset, 4, std::move(id_ev));
    }
}

// ---- impactConfig (staminaCost, manaCost): kiln.gamedata's item config value, its tooltip row following ----
std::string config_path(const std::string& config) {  // Kiln's config names as the game's config ids (as the model reads them)
    static const std::map<std::string, std::string> ids = {{"StaminaCost", "0x727d7f78"}, {"ManaCost", "0xaf236ece"}};
    const auto it = ids.find(config);
    return it == ids.end() ? config : it->second;
}

void bind_impact(const std::shared_ptr<Binding>& b, const std::string& path) {
    GdValueSlot s;
    std::string why;
    if (!gd_item_config_slot(b->entry, path, &s, &why)) return (void)fail(b, why);
    const Field vf = s.f;
    LayerEval ev = [b, vf](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
        const Value v = registry::evaluate(b->rows, to_kiln(vf, nullptr, b->spec, below), b->spec);
        if (!in_range(b->spec, v, why) || !to_game(vf, nullptr, b->spec, v, out, why)) return fail(b, *why);
        return true;
    };
    if (!put_at(b, s.kind, s.id, s.field, -1, s.addr, vf.bytes, std::move(ev))) return (void)fail(b, "the config value is not readable");
    if (!s.tip) return;
    const uintptr_t at = s.addr;
    const uint8_t format = s.tip_format;
    LayerEval tip = [b, vf, at, format](const uint8_t* below, uint8_t, uint8_t* out, std::string* why) {
        uint8_t now[kSlotBytes] = {}, under[kSlotBytes] = {};
        store_effective(at, vf.bytes, now);
        store_below_registry(at, vf.bytes, under);
        if (std::memcmp(now, under, vf.bytes) == 0) {  // the registry changes nothing here: the row as it is below
            std::memcpy(out, below, 4);
            return true;
        }
        int64_t shown = 0;
        if (!gd_ui_scale(format, as_double(decode(vf, now)), &shown)) return *why = "the tooltip's format is unknown", false;
        const int32_t v32 = static_cast<int32_t>(std::clamp<int64_t>(shown, INT32_MIN, INT32_MAX));
        std::memcpy(out, &v32, 4);
        return true;
    };
    put_at(b, s.kind, s.id, -1, -1, s.tip, 4, std::move(tip));
}

// ---- unlock: "always" clears the recipe's knowledge requirement, "game" keeps the game's ----
void bind_unlock(const std::shared_ptr<Binding>& b) {
    const Schema& sc = reg(KILN_CONTENT_RECIPES).schema;
    const auto it = sc.special.find("knowledgeRequirement");
    const uint32_t size = g_registry_type_size("keen::KnowledgeQueryData");
    if (it == sc.special.end() || size == 0 || size > kSlotBytes) return (void)fail(b, "this game build's recipes have no knowledge requirement Kiln can write");
    const uintptr_t at = b->entry.addr + it->second;
    LayerEval ev = [b, at](const uint8_t* below, uint8_t n, uint8_t* out, std::string* why) {
        uint8_t orig[kSlotBytes] = {};
        if (!store_original(at, n, orig)) std::memcpy(orig, below, n);
        const Value v = registry::evaluate(b->rows, Value::text(zero_bytes(below, n) ? "always" : "game"), b->spec);
        if (v.kind != Value::Text || (v.t != "always" && v.t != "game")) return fail(b, *why = "unlock is game or always");
        if (v.t == "always") std::memset(out, 0, n);
        else std::memcpy(out, zero_bytes(below, n) ? orig : below, n);
        return true;
    };
    if (!put(b, -1, kSubUnlock, at, static_cast<uint8_t>(size), std::move(ev))) fail(b, "the recipe is not readable");
}

// ---- one field ----
void bind(const registry::FieldRef& fr, const registry::FieldOutcome& fo) {
    const std::vector<const ChainRow*> rows = registry_rows(fo);
    if (rows.empty()) return;  // only older writers: nothing of the registry's
    const registry::RegistrySpec* rs = g.in->schema->registry(fr.registry);
    const bool raw = registry::is_raw_field(fr.field);
    const FieldSpec* spec = raw ? (fo.raw_spec ? &*fo.raw_spec : nullptr) : rs ? rs->field(fr.field) : nullptr;
    if (!spec) return refuse(fr, rows, "not a field of the schema");
    if (spec->applies == registry::Applies::Restart) return;  // the boot back-end's (S10)
    auto b = std::make_shared<Binding>();
    b->ref = fr;
    for (const ChainRow* r : rows) b->rows.push_back(*r);
    b->spec = *spec;
    b->kind = kind_of(fr.registry);
    if (!b->kind) return refuse(fr, rows, "this registry has no live back-end");
    std::string guid;
    const Entry* e = find_entry(fr.registry, fr.id, &guid);
    if (!e) {
        if (b->kind == KILN_CONTENT_DROPS && !guid.empty()) {  // the game loads drop tables as it needs them
            g.pending_tables.push_back({fr, guid});
            return;
        }
        return refuse(fr, rows, "the entry is not in this world");
    }
    b->entry = *e;
    const Schema& sc = reg(b->kind).schema;
    if (raw) {
        const Field* f = sc.find(fr.field.substr(4));
        if (!f || !f->writable) return refuse(fr, rows, "the member " + fo.raw + " is not a writable member on this game build");
        return bind_scalar(b, f);
    }
    std::string reason;
    if (g.in->regmap->mapping(fr.registry, fr.field, &reason) != rmap::Mapping::Mapped) return refuse(fr, rows, reason);
    const auto rm = g.in->regmap->registries.find(fr.registry);
    if (rm == g.in->regmap->registries.end()) return refuse(fr, rows, "no regmap rows for this registry");
    const auto row = rm->second.rows.find(fr.field);
    if (row == rm->second.rows.end()) return refuse(fr, rows, "no regmap row");
    b->row = row->second;
    const rmap::Row& rw = row->second;
    if (rw.backend != rmap::Backend::Live) return refuse(fr, rows, "this field has no live back-end on this game build");
    const std::string& conv = rw.conv;
    if (conv == "int" || conv == "float" || conv == "bool" || conv == "time" || conv == "enum") {
        const Field* f = rw.paths.empty() ? nullptr : sc.find(rw.paths[0]);
        if (!f || !f->writable) return refuse(fr, rows, "the game member " + (rw.paths.empty() ? std::string("?") : rw.paths[0]) + " is not writable on this game build");
        return bind_scalar(b, f);
    }
    if (conv == "formula") {
        std::vector<const Field*> in;
        for (const std::string& p : rw.paths) {
            const Field* f = sc.find(p);
            if (!f) return refuse(fr, rows, "the game member " + p + " is missing on this game build");
            in.push_back(f);
        }
        if (in.empty() || !registry::formulas::known(rw.formula)) return refuse(fr, rows, "unknown formula " + rw.formula);
        return bind_formula(b, in);
    }
    if (conv == "list" && b->kind == KILN_CONTENT_RECIPES && (fr.field == "inputs" || fr.field == "outputs"))
        return bind_recipe_list(b, fr.field == "inputs" ? KILN_CONTENT_INPUTS : KILN_CONTENT_OUTPUTS);
    if (conv == "inventoryWalk" && b->kind == KILN_CONTENT_DROPS) return bind_drops(b);
    if (conv == "refList" && b->kind == KILN_CONTENT_ITEMS && fr.field == "perks") return bind_perks(b);
    if (conv == "impactConfig" && b->kind == KILN_CONTENT_ITEMS && !rw.paths.empty()) return bind_impact(b, rw.paths[0] + "." + config_path(rw.config));
    if (conv == "unlock" && b->kind == KILN_CONTENT_RECIPES) return bind_unlock(b);
    refuse(fr, rows, "the live back-end does not write " + conv + " fields yet");
}

void put_all() {
    store_drop_owner(registry_owner());
    g.pending_tables.clear();
    g.ordinal = 0;
    for (const auto& [fr, fo] : g.in->result.fields) bind(fr, fo);
}

void report() {
    registry::Result r = g.in->result;
    const size_t first = r.refused.size();
    for (const registry::Refusal& x : g.refused) r.refused.push_back(x);
    for (const StoreSkipped& s : store_skipped()) {
        registry::Refusal x;
        x.mod = s.mod, x.source = s.source, x.reason = s.text;
        r.refused.push_back(std::move(x));
    }
    size_t logged = 0;
    for (size_t i = first; i < r.refused.size() && logged < 50; ++i, ++logged)
        LOGW("registry (live): %s%s", r.refused[i].mod.empty() ? "" : (r.refused[i].mod + ": ").c_str(), r.refused[i].reason.c_str());
    if (r.refused.size() - first > 50) LOGW("registry (live): ... and %zu more (state/registry.json)", r.refused.size() - first - 50);
    g.reported = r;
    g.have_report = true;
    if (g.in->report) g.in->report(r);
}

std::string edit_key(const registry::Edit& e) {
    if (e.entry != registry::EntryOp::None) return e.registry + "#entry";
    if (!e.raw.empty()) return e.registry + "#raw";
    return e.registry + "." + (e.element.on ? e.element.list : registry::generic_path(e.field));
}

// The data batches (source "content/...") of the kept inputs replaced by the sections now; an edit naming what the start
// never decoded, or an entry add / remove the start did not have, refused ("needs a restart").
void recompose(const std::vector<RegistrySection>& sections) {
    std::vector<registry::ModInput> mods = g.in->mods;
    auto is_data = [](const std::string& source) { return source.rfind("content/", 0) == 0; };
    for (registry::ModInput& m : mods) {
        std::set<std::string> before;
        for (const registry::Batch& bt : m.batches)
            if (is_data(bt.source))
                for (const registry::Edit& e : bt.edits) before.insert(edit_key(e));
        std::erase_if(m.batches, [&](const registry::Batch& bt) { return is_data(bt.source); });
        std::erase_if(m.refused, [&](const registry::Refusal& r) { return is_data(r.source); });
        for (const RegistrySection& s : sections) {
            if (s.mod != m.id) continue;
            for (registry::Batch bt : s.batches) {
                std::erase_if(bt.edits, [&](const registry::Edit& e) {
                    const std::string k = edit_key(e);
                    if (before.count(k) || (k.find('#') == std::string::npos && g.in->decoded.count(k))) return false;
                    registry::Refusal r;
                    r.mod = m.id, r.source = bt.source, r.stage = bt.stage, r.n = e.n;
                    r.reason = k + ": needs a restart: the base was read at start; this edit was not applied";
                    g.refused.push_back(r);
                    return true;
                });
                m.batches.push_back(std::move(bt));
            }
            for (const registry::Refusal& r : s.refused) m.refused.push_back(r);
        }
    }
    for (const RegistrySection& s : sections)  // a mod that had no registry content at the start
        if (std::none_of(mods.begin(), mods.end(), [&](const registry::ModInput& m) { return m.id == s.mod; })) {
            registry::Refusal r;
            r.mod = s.mod, r.source = s.source;
            r.reason = "a registry section added after the start needs a restart; this edit was not applied";
            g.refused.push_back(r);
        }
    registry::Result next = registry::compose(*g.in->schema, g.in->base, g.in->old, mods);
    for (const auto& [fr, fo] : next.fields) {
        const registry::RegistrySpec* rs = g.in->schema->registry(fr.registry);
        const FieldSpec* sp = rs ? rs->field(fr.field) : nullptr;
        if (!sp || sp->applies != registry::Applies::Restart) continue;
        auto old = g.in->result.fields.find(fr);
        if (old == g.in->result.fields.end() || !(old->second.result == fo.result))
            LOGI("registry: %s changes at the next start (a restart field)", fr.text().c_str());
    }
    g.in->mods = std::move(mods);
    g.in->result = std::move(next);
}

}  // namespace

KilnOwner registry_owner() {
    std::lock_guard lock(store_mutex());
    if (!g.owner) {
        static const char kLabel[] = "kiln:registry";
        g.owner = g_api ? g_api->owner_create(KilnStr{kLabel, sizeof kLabel - 1}) : static_cast<KilnOwner>(0x7E610001);
        set_owner_label(g.owner, "registry");
    }
    return g.owner;
}

void registry_live_install(RegistryLive in) {
    std::lock_guard lock(store_mutex());
    g.in = std::make_shared<RegistryLive>(std::move(in));
    if (!g.in->schema) g.in->schema = std::make_shared<const registry::Schema>(registry_schema());
    if (!g.in->regmap) g.in->regmap = std::make_shared<const rmap::RegMap>();
    g.kiln_of.clear();
    g.kiln_by_guid.clear();
    for (const auto& [key, guid] : g.in->ids) {
        const size_t bar = key.find('|');
        if (bar != std::string::npos && !guid.empty()) g.kiln_by_guid.emplace(key.substr(0, bar) + "|" + lower(guid), key.substr(bar + 1));
    }
    for (const auto& [key, num] : g.in->nums) {
        const size_t bar = key.find('|');
        if (bar != std::string::npos) g.kiln_of.emplace(num_key(key.substr(0, bar), num), key.substr(bar + 1));
    }
    g.hashes.clear();
    g.applied = false;
    g.have_report = false;
}

bool registry_live_installed() {
    std::lock_guard lock(store_mutex());
    return g.in != nullptr;
}

void registry_live_apply(const std::vector<PackFile>& files) {
    std::lock_guard lock(store_mutex());
    if (!g.in) return;
    const std::vector<RegistrySection> sections = registry_sections(files, *g.in->schema);
    std::map<std::string, uint64_t> hashes;
    for (const RegistrySection& s : sections) hashes[s.rel] = s.hash;
    g.refused.clear(), g.refused_keys.clear();
    if (g.applied && hashes != g.hashes) {
        LOGI("registry: a data pack's registry section changed: composing the registry again");
        recompose(sections);
    }
    g.hashes = std::move(hashes);
    g.applied = true;
    put_all();
    std::string err;
    store_flush(&err);
    if (!err.empty()) LOGW("registry (live): writing: %s", err.c_str());
    report();
}

void registry_live_table(const Entry& table) {
    std::lock_guard lock(store_mutex());
    if (!g.in || g.pending_tables.empty()) return;
    const std::string guid = lower(guid_text(table.guid));
    std::vector<LiveState::Pending> now;
    std::erase_if(g.pending_tables, [&](const LiveState::Pending& p) {
        if (lower(p.guid) != guid) return false;
        now.push_back(p);
        return true;
    });
    for (const LiveState::Pending& p : now)
        if (auto it = g.in->result.fields.find(p.field); it != g.in->result.fields.end()) bind(p.field, it->second);
}

const registry::Result* registry_live_result() {
    std::lock_guard lock(store_mutex());
    return g.have_report ? &g.reported : nullptr;
}

}  // namespace kc
