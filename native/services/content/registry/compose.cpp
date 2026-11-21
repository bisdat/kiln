// SPDX-License-Identifier: MIT
// kiln.content registry stage: composition and conflicts (compose.h).
#include "compose.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace kc::registry {

const char* writer_kind_name(WriterKind k) {
    switch (k) {
        case WriterKind::ArchiveWriter: return "archiveWriter";
        case WriterKind::OldPatch: return "oldPatch";
        case WriterKind::OldPack: return "oldPack";
        case WriterKind::Code: return "code";
        default: return "registry";
    }
}

const char* certainty_name(Certainty c) {
    switch (c) {
        case Certainty::Named: return "named";
        case Certainty::BestEffort: return "bestEffort";
        default: return "certain";
    }
}

const char* line_kind_name(LineKind k) {
    switch (k) {
        case LineKind::Overridden: return "overridden";
        case LineKind::Composed: return "composed";
        case LineKind::ComposedOn: return "composedOn";
        case LineKind::Exclusive: return "exclusive";
        case LineKind::Dropped: return "dropped";
        default: return "yielded";
    }
}

namespace {

// One spelling per value as operator== sees it (1 and 1.0 are one number), for a hashed remove.
std::string value_key(const Value& v) {
    if (v.numeric()) {
        const double d = v.as_double() == 0 ? 0.0 : v.as_double();
        char buf[40];
        std::snprintf(buf, sizeof buf, "n%.17g", d);
        return buf;
    }
    switch (v.kind) {
        case Value::Bool: return v.b ? "b1" : "b0";
        case Value::Text: return "t" + v.t;
        case Value::List: return "l" + v.to_json().dump();
        case Value::Struct: return "s" + v.to_json().dump();
        default: return "z";
    }
}

// A list op on a list value. Flags (f->type Flags) stay a set in the schema's order: append adds names not there yet.
Value list_step(const ListOp op, const std::vector<Value>& values, const Value& v, const FieldSpec* f = nullptr) {
    std::vector<Value> l = v.kind == Value::List ? v.list : std::vector<Value>{};
    if (f && f->type == FieldType::Flags) {
        std::set<std::string> on;
        if (op != ListOp::Clear && op != ListOp::Replace)
            for (const Value& x : l) on.insert(x.t);
        if (op == ListOp::Replace || op == ListOp::Append)
            for (const Value& x : values) on.insert(x.t);
        if (op == ListOp::Remove)
            for (const Value& x : values) on.erase(x.t);
        std::vector<Value> names;
        for (const std::string& n : f->enum_values)
            if (on.count(n)) names.push_back(Value::text(n));
        return Value::of_list(std::move(names));
    }
    if (op == ListOp::Clear) l.clear();
    else if (op == ListOp::Replace) l = values;
    else if (op == ListOp::Append) l.insert(l.end(), values.begin(), values.end());
    else {
        std::unordered_set<std::string> gone;
        for (const Value& x : values) gone.insert(value_key(x));
        std::erase_if(l, [&](const Value& x) { return gone.count(value_key(x)) != 0; });
    }
    return Value::of_list(std::move(l));
}

// An element edit on a list value ("inputs[2].count", "inputs[*].count", "perks[1]"): op on that leaf (or that whole element) of
// the element(s), typed by el (the leaf's spec, or the element's). false + *why: refused (no such element, no number there,
// a result out of range).
bool element_step(const ElementRef& r, const Op& op, const FieldSpec& el, const Value& v, Value* next, std::string* why) {
    if (v.kind != Value::List) return *why = r.list + " has no list here", false;
    std::vector<Value> l = v.list;
    auto one = [&](Value& x) {
        Value* at = &x;
        Value fresh;
        size_t k = 0;
        if (!r.leaf.empty()) {
            if (x.kind != Value::Struct) return *why = r.list + ": its elements have no fields here", false;
            while (k < x.keys.size() && x.keys[k] < r.leaf) ++k;
            at = k < x.keys.size() && x.keys[k] == r.leaf ? &x.list[k] : &fresh;
        }
        if (op.relative() && !at->numeric()) return *why = el.name + " has no number to change here", false;
        Value nv;
        if (!op_result(op, *at, el, &nv, why)) return false;
        if (nv.numeric() && ((el.min && nv.as_double() < *el.min) || (el.max && nv.as_double() > *el.max)))
            return *why = el.name + ": the result " + nv.text_of() + " is outside the field's range", false;
        if (at != &fresh) {
            *at = std::move(nv);
        } else {  // a leaf the element did not have: in its sorted place
            x.keys.insert(x.keys.begin() + static_cast<std::ptrdiff_t>(k), r.leaf);
            x.list.insert(x.list.begin() + static_cast<std::ptrdiff_t>(k), std::move(nv));
        }
        return true;
    };
    if (r.all) {
        for (Value& x : l)
            if (!one(x)) return false;
    } else {
        if (r.index >= l.size()) return *why = r.list + " has no element " + std::to_string(r.index) + " here (it has " + std::to_string(l.size()) + ")", false;
        if (!one(l[r.index])) return false;
    }
    *next = Value::of_list(std::move(l));
    return true;
}

}  // namespace

Value evaluate(const std::vector<ChainRow>& rows, const Value& below, const FieldSpec& f) {
    Value v = below;
    for (const ChainRow& r : rows) {
        if (r.element.on) {
            Value nv;
            std::string why;
            if (r.element_spec && element_step(r.element, r.op, *r.element_spec, v, &nv, &why)) v = std::move(nv);
            continue;
        }
        v = r.list == ListOp::None ? apply_op(r.op, v, f) : list_step(r.list, r.list_values, v, &f);
    }
    return v;
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct EntryState {
    std::string id;
    std::set<std::string> traits;
    std::map<std::string, Value> base;
    bool removed = false;
    std::string removed_by, removed_in;
    bool added = false;  // a mod's entry: its raw members are its like entry's
    std::string like;
    bool retired = false;  // the ids ledger retired this id: never added again, never edited
};

// The stage's state. A final's own view is an overlay over the post-alter state (parent): reads fall through, writes stay in
// the overlay, so a mod's final costs what it touches, not a copy of everything.
struct State {
    const State* parent = nullptr;
    std::map<std::string, std::vector<EntryState>> regs;  // the root's entries, in each registry's order
    std::map<EntryRef, size_t> at;                        // the root's index into regs
    std::map<EntryRef, EntryState> over;                  // an overlay's changed entries
    std::map<FieldRef, FieldOutcome> fields;              // this level's fields (an overlay: the ones it touched)

    const EntryState* entry(const std::string& reg, const std::string& id) const {
        if (parent) {
            const auto it = over.find({reg, id});
            return it != over.end() ? &it->second : parent->entry(reg, id);
        }
        const auto it = at.find({reg, id});
        return it == at.end() ? nullptr : &regs.at(reg)[it->second];
    }
    EntryState* entry(const std::string& reg, const std::string& id) {
        if (parent) {
            const auto it = over.find({reg, id});
            if (it != over.end()) return &it->second;
            const EntryState* p = parent->entry(reg, id);
            return p ? &over.emplace(EntryRef{reg, id}, *p).first->second : nullptr;
        }
        const auto it = at.find({reg, id});
        return it == at.end() ? nullptr : &regs[reg][it->second];
    }
    // every entry of a registry, in its order, as this level sees it
    void each_entry(const std::string& reg, const std::function<void(const EntryState&)>& fn) const {
        if (parent) {
            parent->each_entry(reg, [&](const EntryState& e) {
                const auto it = over.find({reg, e.id});
                fn(it != over.end() ? it->second : e);
            });
            return;
        }
        if (const auto it = regs.find(reg); it != regs.end())
            for (const EntryState& e : it->second) fn(e);
    }
    const FieldOutcome* find_field(const FieldRef& f) const {
        const auto it = fields.find(f);
        if (it != fields.end()) return &it->second;
        return parent ? parent->find_field(f) : nullptr;
    }
    Value base_of(const FieldRef& f) const {
        const EntryState* e = entry(f.registry, f.id);
        if (!e) return {};
        const auto b = e->base.find(f.field);
        return b == e->base.end() ? Value{} : b->second;
    }
    Value current(const FieldRef& f) const {
        const FieldOutcome* fo = find_field(f);
        return fo ? fo->result : base_of(f);
    }
    FieldOutcome& touch(const FieldRef& f) {
        auto [it, fresh] = fields.try_emplace(f);
        if (fresh) {
            if (const FieldOutcome* p = parent ? parent->find_field(f) : nullptr) it->second = *p;
            else it->second.result = it->second.base = base_of(f);
        }
        return it->second;
    }
    void add_entry(const std::string& reg, EntryState e) {  // the root only: entries are added in stage add
        auto& list = regs[reg];
        at[{reg, e.id}] = list.size();
        list.push_back(std::move(e));
    }

    // The Advanced tier: the member at raw path key of an entry on this build, through the base model (once per place; an
    // added entry through its like entry). nullptr + *why: it does not resolve there.
    std::map<FieldRef, std::optional<RawMember>> raw_members;
    std::map<FieldRef, std::string> raw_why;
    bool cached_raw(const FieldRef& ref, const RawMember** m, std::string* why) const {
        if (const auto it = raw_members.find(ref); it != raw_members.end()) {
            *m = it->second ? &*it->second : nullptr;
            if (!it->second) *why = raw_why.at(ref);
            return true;
        }
        return parent && parent->cached_raw(ref, m, why);
    }
    const RawMember* raw_member(const BaseModel& b, const std::string& reg, const std::string& id, const std::string& key, std::string* why) {
        const FieldRef ref{reg, id, raw_field(key)};
        const RawMember* known = nullptr;
        if (cached_raw(ref, &known, why)) return known;
        std::optional<RawMember> m;
        std::string w;
        std::string game = id;  // the game entry whose data it is
        for (int hops = 0; hops < 64; ++hops) {
            const EntryState* e = entry(reg, game);
            if (!e || !e->added) break;
            if (e->like.empty()) {
                w = "an entry added without \"like\" has no game data for raw paths";
                game.clear();
                break;
            }
            game = e->like;
        }
        if (!game.empty()) {
            if (!b.raw) w = "this Kiln resolves no raw paths here";
            else if (!(m = b.raw(reg, game, key, &w)) && w.empty()) w = "no such member";
        }
        if (m) {
            if (EntryState* e = entry(reg, id)) e->base.try_emplace(ref.field, m->value);
        } else {
            raw_why[ref] = w;
            *why = w;
        }
        const auto& slot = raw_members[ref] = std::move(m);
        return slot ? &*slot : nullptr;
    }
};

class StateView final : public View {
public:
    explicit StateView(const State& s) : s_(s) {}
    bool exists(const std::string& reg, const std::string& id) const override {
        const EntryState* e = s_.entry(reg, id);
        return e && !e->removed;
    }
    std::optional<Value> get(const FieldRef& f) const override {
        if (!exists(f.registry, f.id)) return std::nullopt;
        Value v = s_.current(f);
        if (v.kind == Value::None) return std::nullopt;
        return v;
    }
    std::vector<std::string> ids(const std::string& reg) const override {
        std::vector<std::string> out;
        s_.each_entry(reg, [&](const EntryState& e) {
            if (!e.removed) out.push_back(e.id);
        });
        return out;
    }
    bool has_trait(const std::string& reg, const std::string& id, const std::string& t) const override {
        const EntryState* e = s_.entry(reg, id);
        return e && e->traits.count(t);
    }

private:
    const State& s_;
};

struct Ctx {
    const Schema& schema;
    Result& out;
    const BaseModel& base;
    // (registry, Kiln field) -> the raw keys behind it that Kiln converts (BaseModel::raw_cover): one place on two scales
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> converted;
    // references: why an entry's add was refused (the cause a reference to it names), and who added each entry
    std::map<EntryRef, std::string> refused_add;
    struct Adder {
        std::string mod, source;
        uint32_t n = 0;
    };
    std::map<EntryRef, Adder> adders;
};

// A "where" condition on one value: ">=1", "<=2", "!=x", "==x", ">1", "<1", or a plain value (equal; names ignore case).
bool cond_ok(const Value& v, const json& c) {
    if (c.is_boolean()) return v.kind == Value::Bool && v.b == c.get<bool>();
    if (c.is_number()) return v.numeric() && v.as_double() == c.get<double>();
    if (!c.is_string()) return false;
    const std::string s = c.get<std::string>();
    static const char* const ops[] = {">=", "<=", "!=", "==", ">", "<"};
    for (const char* op : ops) {
        const size_t n = std::char_traits<char>::length(op);
        if (s.compare(0, n, op) != 0) continue;
        const std::string rest = s.substr(n);
        char* end = nullptr;
        const double x = std::strtod(rest.c_str(), &end);
        const bool num = !rest.empty() && end && *end == 0 && v.numeric();
        const double a = v.as_double();
        if (n == 2 && op[0] == '>') return num && a >= x;
        if (n == 2 && op[0] == '<') return num && a <= x;
        if (op[0] == '>') return num && a > x;
        if (op[0] == '<') return num && a < x;
        const bool eq = num ? a == x : (v.kind == Value::Text && lower(v.t) == lower(rest));
        return op[0] == '=' ? eq : !eq;
    }
    return v.kind == Value::Text && lower(v.t) == lower(s);
}

// The entries a selector names on view v. false: refused (*why). An exact id another edit removed: *dropped_by, true.
bool select(const View& v, const Schema& schema, const State& s, const std::string& reg, const Selector& sel, std::vector<std::string>* ids,
            std::string* why, std::string* dropped_by) {
    ids->clear();
    if (sel.where.is_object())
        for (const auto& [k, c] : sel.where.items())
            if (!schema.registry(reg)->field(k)) return *why = "where: " + reg + " has no field \"" + k + "\"", false;
    // D3: the old debug-name forms keep working: "Material_T1_Wood_Logs" names "game:material_t1_wood_logs"
    // Kiln ids are lower case: an exact id selects without case, as a glob does
    const std::string id = sel.id.find(':') == std::string::npos && sel.id != "*" ? "game:" + lower(sel.id) : lower(sel.id);
    auto keep = [&](const std::string& e) {
        if (sel.where.is_object())
            for (const auto& [k, c] : sel.where.items()) {
                const auto x = v.get({reg, e, k});
                if (!x || !cond_ok(*x, c)) return false;
            }
        for (const std::string& t : sel.has)
            if (!v.has_trait(reg, e, t)) return false;
        for (const std::string& t : sel.has_not)
            if (v.has_trait(reg, e, t)) return false;
        return true;
    };
    if (id.find_first_of("*?") == std::string::npos) {
        if (!v.exists(reg, id)) {
            const EntryState* e = s.entry(reg, id);
            if (e && !e->removed_in.empty()) return *why = id + " was removed by game build " + e->removed_in + "; this edit was not applied", false;
            if (e && e->removed && !e->retired) return *dropped_by = e->removed_by, true;
            return *why = "no entry " + id + " in " + reg, false;
        }
        if (keep(id)) ids->push_back(id);
        return true;
    }
    for (const std::string& e : v.ids(reg))
        if (glob_ci(id, e) && keep(e)) ids->push_back(e);
    return true;
}

// One write of an edit into one place: a Kiln field, or a raw member (the Advanced tier).
struct Target {
    FieldRef ref;
    const FieldSpec* spec = nullptr;  // the schema's field, or the raw member's own spec on this build (both outlive the stage)
    Op op;
    ListOp list = ListOp::None;
    std::vector<Value> list_values;
    std::string raw;         // the raw path as written (an Advanced-tier write), else empty
    std::string conv_field;  // a raw member behind a Kiln field Kiln converts: that field (one place, two scales)
    const ElementRef* element = nullptr;  // an element edit of the list at ref ("inputs[2].count")
    std::string path;                     // ...as the edit names it
    std::optional<FieldSpec> element_spec;  // ...its leaf's spec (or the plain element's)
};

bool absolute(const Target& t) { return t.element ? false : t.list == ListOp::None ? t.op.set : (t.list == ListOp::Clear || t.list == ListOp::Replace); }
bool absolute(const ChainRow& r) { return r.element.on ? false : r.list == ListOp::None ? r.op.set : (r.list == ListOp::Clear || r.list == ListOp::Replace); }
Certainty certainty_of(WriterKind k) { return k == WriterKind::ArchiveWriter ? Certainty::Named : Certainty::Certain; }

// The place a target names, for messages: "items[game:x].maxStack", "items[game:x].damageSetup.dmgMod (raw)".
std::string place_text(const Target& t) {
    if (t.element) return t.ref.registry + "[" + t.ref.id + "]." + t.path;
    return t.raw.empty() || !is_raw_field(t.ref.field) ? t.ref.text() : t.ref.registry + "[" + t.ref.id + "]." + t.raw + " (raw)";
}

// The value an edit makes of cur. false: refused (*why).
bool next_value(const Target& t, const Value& cur, Value* next, std::string* why) {
    const FieldSpec& f = *t.spec;
    if (t.element) return element_step(*t.element, t.op, *t.element_spec, cur, next, why);
    if (t.list == ListOp::None) {
        if (t.op.relative() && !cur.numeric()) return *why = f.name + " has no number to change here", false;
        if (!op_result(t.op, cur, f, next, why)) return false;
        if (next->numeric() && ((f.min && next->as_double() < *f.min) || (f.max && next->as_double() > *f.max)))
            return *why = f.name + ": the result " + next->text_of() + " is outside the field's range", false;
        return true;
    }
    *next = list_step(t.list, t.list_values, cur, &f);
    if (f.max_items && next->list.size() > f.max_items)
        return *why = f.name + " would hold " + std::to_string(next->list.size()) + " values; it holds at most " + std::to_string(f.max_items), false;
    return true;
}

struct Who {
    std::string mod, source;
    Stage stage;
};

// One edit's writes over its entries: all or nothing. report: false while a mod's final runs on its own scratch copy.
bool apply_targets(Ctx& c, State& s, const Who& w, const Edit& e, const std::vector<Target>& ts, bool report, std::string* why) {
    struct Step {
        const Target* t = nullptr;
        Value next;
        bool yielded = false, line = false;
        LineKind kind = LineKind::Overridden;
        std::string other;
        Certainty certainty = Certainty::Certain;
    };
    auto refuse_line = [&](const Target& t, const std::string& winner, Certainty cert, const std::string& text) {
        if (report) c.out.lines.push_back({LineKind::Exclusive, t.ref, winner, w.mod, text, cert});
        *why = text;
        return false;
    };
    std::vector<Step> steps;
    for (const Target& t : ts) {
        // one place on two scales (a raw member behind a field Kiln converts): never composed; the later edit is refused
        const ChainRow* scale = nullptr;
        std::string scale_place;
        if (!t.conv_field.empty()) {
            const FieldOutcome* o = s.find_field({t.ref.registry, t.ref.id, t.conv_field});
            if (o && !o->chain.empty()) scale = &o->chain.back(), scale_place = t.ref.registry + "." + t.conv_field;
        } else if (t.raw.empty()) {
            const auto conv = c.converted.find({t.ref.registry, t.ref.field});
            if (conv != c.converted.end())
                for (const std::string& key : conv->second) {
                    const FieldOutcome* o = s.find_field({t.ref.registry, t.ref.id, raw_field(key)});
                    if (!o || o->chain.empty()) continue;
                    scale = &o->chain.back(), scale_place = "raw path " + (o->raw.empty() ? key : o->raw);
                    break;
                }
        }
        if (scale) {
            const std::string text = "Conflict (one place, two scales): " + place_text(t) + " and " + scale_place + " are one place in the game, which Kiln converts " +
                                     "between them; " + scale->who + " writes " + scale_place + " there and the two cannot be composed on one value: " + w.mod +
                                     "'s later edit is refused";
            return refuse_line(t, scale->who, certainty_of(scale->kind), text);
        }
        Step st;
        st.t = &t;
        const FieldOutcome* fo = s.find_field(t.ref);
        const Value cur = fo ? fo->result : s.base_of(t.ref);
        // another writer: another mod's registry edit, an old path of another mod, an archive-writer package (the mod's own
        // older path is no conflict with itself)
        auto other = [&](const ChainRow& r) { return r.mod != w.mod || r.mod.empty(); };
        const ChainRow* prev = nullptr;        // the row just before this edit
        const ChainRow* last_other = nullptr;  // the latest other writer that is not the archive writer (its value is base, D1)
        const ChainRow* excl = nullptr;        // the latest other writer whose edit is exclusive
        bool earlier = false;                  // an earlier writer for yield: not the archive writer, not the mod itself
        if (fo)
            for (const ChainRow& r : fo->chain) {
                prev = &r;
                if (!other(r) || r.kind == WriterKind::ArchiveWriter) continue;
                earlier = true;
                last_other = &r;
                if (r.exclusive) excl = &r;
            }
        if (e.yield && earlier) {
            st.yielded = true;
            steps.push_back(std::move(st));
            continue;
        }
        if (!next_value(t, cur, &st.next, why)) return *why = t.ref.id + ": " + *why, false;
        if (last_other && (e.exclusive || excl)) {
            const bool agreed = absolute(t) && absolute(*last_other) && st.next == cur;
            const ChainRow& named = e.exclusive ? *last_other : *excl;  // the conflict is with the exclusive edit
            if (!agreed)
                return refuse_line(t, named.who, certainty_of(named.kind),
                                   "Conflict (exclusive): " + named.who + (e.exclusive ? " also writes " : " wrote ") + place_text(t) +
                                       (e.exclusive ? "; the later edit is refused" : " exclusively; the later edit is refused"));
        }
        if (prev && other(*prev)) {  // a line when this edit follows another writer's (not after the mod's own)
            if (absolute(t)) {
                st.line = !(st.next == cur);
                st.kind = LineKind::Overridden;
            } else {
                st.line = true;
                st.kind = absolute(*prev) ? LineKind::ComposedOn : LineKind::Composed;
            }
            st.other = prev->who;
            st.certainty = certainty_of(prev->kind);
        }
        steps.push_back(std::move(st));
    }
    for (Step& st : steps) {
        const Target& t = *st.t;
        if (st.yielded) {
            if (report)
                c.out.lines.push_back({LineKind::Yielded, t.ref, "", w.mod, "yielded: " + place_text(t) + " was set by an earlier writer; " + w.mod + "'s edit is not applied there"});
            continue;
        }
        FieldOutcome& fo = s.touch(t.ref);
        if (is_raw_field(t.ref.field)) {
            if (fo.raw.empty()) fo.raw = t.raw;
            if (!fo.raw_spec) fo.raw_spec = *t.spec;
        }
        const Value before = fo.result;
        ChainRow r;
        r.kind = WriterKind::Registry;
        r.raw = t.raw;
        r.mod = r.who = w.mod;
        r.stage = w.stage;
        r.source = w.source;
        r.n = e.n;
        r.op = t.op;
        r.list = t.list;
        r.list_values = t.list_values;
        if (t.element) r.element = *t.element, r.element_spec = t.element_spec;
        r.exclusive = e.exclusive;
        r.after = st.next;
        fo.chain.push_back(std::move(r));
        fo.result = st.next;
        fo.winner = w.mod;
        if (report && st.line) {
            const std::string place = place_text(t);
            std::string text = st.kind == LineKind::Overridden
                                   ? place + ": " + w.mod + " sets " + st.next.text_of() + " over " + st.other + "'s " + before.text_of() + " (the later one wins)"
                               : st.kind == LineKind::ComposedOn
                                   ? place + ": " + w.mod + "'s change composed on " + st.other + "'s value: " + before.text_of() + " -> " + st.next.text_of()
                                   : place + ": " + w.mod + "'s change stacks on " + st.other + "'s: " + before.text_of() + " -> " + st.next.text_of();
            c.out.lines.push_back({st.kind, t.ref, w.mod, st.other, std::move(text), st.certainty});
        }
    }
    return true;
}

// A Kiln field edit's writes over its entries.
std::vector<Target> field_targets(const FieldSpec& f, const Edit& e, const std::vector<std::string>& ids) {
    std::vector<Target> ts;
    std::optional<FieldSpec> el;  // an element edit: f is the list; the leaf's spec, or the plain element's
    if (e.element.on) {
        if (!e.element.leaf.empty()) {
            if (const FieldSpec* leaf = f.element_field(e.element.leaf)) el = *leaf;
        } else {
            el = f;
            el->type = f.element;
        }
        if (el) el->name = e.field;
    }
    for (const std::string& id : ids) {
        Target t;
        t.ref = {e.registry, id, e.element.on ? e.element.list : e.field};
        t.spec = &f;
        if (e.element.on) t.element = &e.element, t.path = e.field, t.element_spec = el;
        t.op = e.op;
        t.list = e.list;
        t.list_values = e.list_values;
        ts.push_back(std::move(t));
    }
    return ts;
}

// One raw path of one entry as a write: a covered member is its Kiln field's place; any other member is resolved on this
// build and typed by its own spec. false + *why: refused.
bool raw_target(Ctx& c, State& s, const std::string& reg, const std::string& id, const RawWrite& rw, Target* t, std::string* why) {
    const BaseModel::Cover* cover = nullptr;
    if (const auto r = c.base.raw_cover.find(reg); r != c.base.raw_cover.end())
        if (const auto k = r->second.find(rw.key); k != r->second.end()) cover = &k->second;
    t->raw = rw.path;
    std::string err;
    if (cover && !cover->converted) {
        const FieldSpec* f = c.schema.registry(reg)->field(cover->field);
        if (!f) return *why = id + ": raw path \"" + rw.path + "\" maps to " + reg + "." + cover->field + ", which the schema does not have", false;
        if (f->read_only) return *why = id + ": raw path \"" + rw.path + "\" is the member of " + reg + "." + cover->field + ", which is read-only", false;
        if (f->type == FieldType::List) return *why = id + ": raw path \"" + rw.path + "\" is the list " + reg + "." + cover->field + ": change it with list ops on that field", false;
        t->ref = {reg, id, cover->field};
        t->spec = f;
        if (!op_of(rw.op, *f, &t->op, &err)) return *why = id + ": raw path \"" + rw.path + "\" (" + reg + "." + cover->field + "): " + err, false;
        return true;
    }
    const RawMember* m = s.raw_member(c.base, reg, id, rw.key, &err);
    if (!m)
        return *why = id + ": raw path \"" + rw.path + "\" does not resolve on this game build (" + err +
                      "); raw paths are the Advanced tier, checked per entry: this edit was not applied", false;
    if (m->spec.read_only) return *why = id + ": raw path \"" + rw.path + "\" is read-only on this game build", false;
    t->ref = {reg, id, raw_field(rw.key)};
    t->spec = &m->spec;
    if (cover) t->conv_field = cover->field;
    if (!op_of(rw.op, m->spec, &t->op, &err)) return *why = id + ": raw path \"" + rw.path + "\": " + err, false;
    return true;
}

void refuse(Ctx& c, const Who& w, const Edit& e, const std::string& why) { c.out.refused.push_back({w.mod, w.source, w.stage, e.n, why, ""}); }

// A value a field can take (code batches bypass the IR reader's checks): its type, its range, a list's size.
bool value_fits(const Value& v, const FieldSpec& f, std::string* why, bool element = false) {
    const FieldType t = element ? f.element : f.type;
    auto range = [&](double d) {
        if (!std::isfinite(d)) return *why = f.name + ": not a finite number", false;
        if ((f.min && d < *f.min) || (f.max && d > *f.max)) return *why = f.name + ": " + v.text_of() + " is outside the field's range", false;
        return true;
    };
    switch (t) {
        case FieldType::Int:
            if (v.kind != Value::Int) return *why = f.name + ": a whole number is expected", false;
            return range(static_cast<double>(v.i));
        case FieldType::Float:
            if (!v.numeric()) return *why = f.name + ": a number is expected", false;
            return range(v.as_double());
        case FieldType::Bool:
            return v.kind == Value::Bool || (*why = f.name + ": true or false is expected", false);
        case FieldType::Enum:
            if (v.kind == Value::Text && std::find(f.enum_values.begin(), f.enum_values.end(), v.t) != f.enum_values.end()) return true;
            return *why = f.name + ": one of the schema's names is expected", false;
        case FieldType::Id:
            return (v.kind == Value::Text && v.t.find(':') != std::string::npos) || (*why = f.name + ": an id is expected", false);
        case FieldType::Flags:
            if (v.kind != Value::List) return *why = f.name + ": a list of names is expected", false;
            for (const Value& x : v.list)
                if (x.kind != Value::Text || std::find(f.enum_values.begin(), f.enum_values.end(), x.t) == f.enum_values.end())
                    return *why = f.name + ": one of the schema's names is expected", false;
            return true;
        case FieldType::Struct:
            if (v.kind != Value::Struct) return *why = f.name + ": an element with fields is expected", false;
            for (size_t k = 0; k < v.keys.size(); ++k) {
                const FieldSpec* leaf = f.element_field(v.keys[k]);
                if (!leaf) return *why = f.name + ": an element has no field \"" + v.keys[k] + "\"", false;
                if (!value_fits(v.list[k], *leaf, why)) return false;
            }
            return true;
        default:
            if (element || v.kind != Value::List) return *why = f.name + ": a list is expected", false;
            if (f.max_items && v.list.size() > f.max_items) return *why = f.name + ": at most " + std::to_string(f.max_items) + " values", false;
            for (const Value& x : v.list)
                if (!value_fits(x, f, why, true)) return false;
            return true;
    }
}

// The field an edit's report lines name: its Kiln field, or its first raw path's chain.
std::string edit_field(const Edit& e) { return e.raw.empty() ? e.field : raw_field(e.raw.front().key); }

// One edit at its mod's turn on state s. resolved: the entries a final edit named on its mod's own copy (else resolved here).
void run_edit(Ctx& c, State& s, const Who& w, const Edit& e, bool report, const std::vector<std::string>* resolved) {
    const RegistrySpec* reg = c.schema.registry(e.registry);
    if (!reg) return refuse(c, w, e, "no registry \"" + e.registry + "\"");
    if (e.entry == EntryOp::Add) {  // all or nothing: the entry and every field it is given, or none of it
        if (w.stage != Stage::Add) return refuse(c, w, e, "an entry is added in stage add");
        if (e.add_id.rfind(w.mod + ":", 0) != 0) return refuse(c, w, e, "an added entry's id is \"" + w.mod + ":<name>\"");
        if (lower(e.add_id) != e.add_id) return refuse(c, w, e, "an added entry's id is lower case (Kiln ids are), not \"" + e.add_id + "\"");
        if (s.entry(e.registry, e.add_id)) return refuse(c, w, e, "the id " + e.add_id + " is taken (ids are never reused)");
        auto refuse_add = [&](const std::string& why) {  // a reference to this id names why it is not there
            c.refused_add.try_emplace({e.registry, e.add_id}, why);
            refuse(c, w, e, why);
        };
        std::vector<std::pair<const FieldSpec*, Value>> given;
        for (const auto& [k, v] : e.add_fields) {
            const FieldSpec* f = reg->field(k);
            std::string why;
            if (!f) return refuse_add(e.registry + " has no field \"" + k + "\"; the entry is not added");
            if (f->read_only) return refuse_add(e.registry + "." + k + " is read-only; the entry is not added");
            if (!value_fits(v, *f, &why)) return refuse_add(why + "; the entry is not added");
            given.emplace_back(f, v);
        }
        EntryState n;
        n.id = e.add_id;
        n.added = true;
        n.like = lower(e.like);
        if (!n.like.empty()) {
            const EntryState* l = s.entry(e.registry, n.like);
            if (!l || l->removed) return refuse_add("like: no entry " + e.like + " in " + e.registry);
            n.traits = l->traits;
            n.base = l->base;
            for (auto it = s.fields.lower_bound({e.registry, n.like, ""}); it != s.fields.end() && it->first.registry == e.registry && it->first.id == n.like; ++it)
                n.base[it->first.field] = it->second.result;
        }
        s.add_entry(e.registry, std::move(n));
        if (report) c.out.added.push_back({e.registry, e.add_id});
        c.adders[{e.registry, e.add_id}] = {w.mod, w.source, e.n};
        for (const auto& [f, v] : given) {  // a fresh entry: no other writer, nothing to refuse
            Edit set;
            set.n = e.n;
            set.registry = e.registry;
            set.field = f->name;
            set.op.set = true;
            set.op.value = v;
            std::string why;
            apply_targets(c, s, w, set, field_targets(*f, set, {e.add_id}), report, &why);
        }
        return;
    }
    if (w.stage == Stage::Add) return refuse(c, w, e, "stage add adds entries; field edits belong in alter or final");
    if (e.entry != EntryOp::Remove && e.raw.empty() && !reg->field(e.element.on ? e.element.list : e.field)) return refuse(c, w, e, e.registry + " has no field \"" + e.field + "\"");
    if (e.element.on && !reg->field(e.element.list)->element_field(e.element.leaf) && !e.element.leaf.empty())
        return refuse(c, w, e, e.registry + "." + e.element.list + "'s elements have no field \"" + e.element.leaf + "\"");
    std::vector<std::string> ids;
    if (resolved) {  // a final edit: entries another mod's final removed since are dropped
        for (const std::string& id : *resolved) {
            const EntryState* x = s.entry(e.registry, id);
            if (x && !x->removed) ids.push_back(id);
            else if (x && report)
                c.out.lines.push_back({LineKind::Dropped, {e.registry, id, edit_field(e)}, x->removed_by, w.mod,
                                       "dropped: " + id + " was removed by " + x->removed_by + "; " + w.mod + "'s edit into it is not applied"});
        }
    } else {
        std::string why, dropped_by;
        if (!select(StateView(s), c.schema, s, e.registry, e.select, &ids, &why, &dropped_by)) return refuse(c, w, e, why);
        if (!dropped_by.empty()) {
            if (report)
                c.out.lines.push_back({LineKind::Dropped, {e.registry, e.select.id, edit_field(e)}, dropped_by, w.mod,
                                       "dropped: " + e.select.id + " was removed by " + dropped_by + "; " + w.mod + "'s edit into it is not applied"});
            return;
        }
    }
    if (e.entry == EntryOp::Remove) {
        for (const std::string& id : ids) {
            EntryState* x = s.entry(e.registry, id);
            if (!x || x->removed) continue;
            x->removed = true;
            x->removed_by = w.mod;
            if (report) c.out.removed.push_back({e.registry, id});
        }
        return;
    }
    std::vector<Target> ts;
    if (!e.raw.empty()) {  // the Advanced tier: every path of every entry resolves, or the edit is refused whole
        for (const std::string& id : ids)
            for (const RawWrite& rw : e.raw) {
                Target t;
                std::string why;
                if (!raw_target(c, s, e.registry, id, rw, &t, &why)) return refuse(c, w, e, why);
                ts.push_back(std::move(t));
            }
    } else {
        ts = field_targets(*reg->field(e.element.on ? e.element.list : e.field), e, ids);
    }
    std::string why;
    if (!apply_targets(c, s, w, e, ts, report, &why)) refuse(c, w, e, why);
}

void run_mod(Ctx& c, State& s, const ModInput& m, Stage stage) {
    std::string bad_id;
    if (!mod_id_ok(m.id, &bad_id)) {  // "game", "kiln", or no Kiln mod id: it mints no ids and edits nothing
        auto refuse_all = [&](const Batch& b) {
            for (const Edit& e : b.edits) refuse(c, {m.id, b.source, stage}, e, "mod id " + bad_id);
        };
        for (const Batch& b : m.batches)
            if (b.stage == stage) refuse_all(b);
        if (m.code) {
            Batch cb;
            cb.source = "code";
            m.code(stage, StateView(s), &cb);
            refuse_all(cb);
        }
        return;
    }
    for (const Batch& b : m.batches) {
        if (b.stage != stage) continue;
        const Who w{m.id, b.source, stage};
        if (b.mod != m.id) {
            for (const Edit& e : b.edits) refuse(c, w, e, "a batch of mod " + b.mod + " in " + m.id + "'s place");
            continue;
        }
        for (const Edit& e : b.edits) run_edit(c, s, w, e, true, nullptr);
    }
    if (m.code) {
        Batch cb;
        cb.mod = m.id;
        cb.stage = stage;
        cb.source = "code";
        m.code(stage, StateView(s), &cb);
        const Who w{m.id, cb.source, stage};
        for (const Edit& e : cb.edits) run_edit(c, s, w, e, true, nullptr);
    }
}

}  // namespace

namespace {

// ---- references (mod-dev feedback adopted for P1) ----
// An entry whose reference (an id field, a list of ids, an id leaf of a list's elements, into a registry of this schema)
// names a refused, removed, retired or missing entry is refused too, per entry, with the cause chain. Runs after the stage;
// a refusal can make another entry's reference bad, so it runs until nothing changes.

using Ref = std::pair<std::string, std::string>;  // the place in the value ("inputs[0].item"), the id as written

void refs_of(const Schema& schema, const FieldSpec& f, const Value& v, const std::string& path, std::vector<Ref>* out) {
    auto known = [&](const std::string& reg) { return !reg.empty() && schema.registry(reg); };
    if (f.type == FieldType::Id) {
        if (v.kind == Value::Text && known(f.ref)) out->emplace_back(path, v.t);
        return;
    }
    if (f.type != FieldType::List || v.kind != Value::List) return;
    for (size_t i = 0; i < v.list.size(); ++i) {
        const Value& x = v.list[i];
        const std::string at = path + "[" + std::to_string(i) + "]";
        if (f.element == FieldType::Id && x.kind == Value::Text && known(f.ref)) out->emplace_back(at, x.t);
        if (f.element == FieldType::Struct && x.kind == Value::Struct)
            for (size_t k = 0; k < x.keys.size(); ++k)
                if (const FieldSpec* leaf = f.element_field(x.keys[k]); leaf && leaf->type == FieldType::Id && x.list[k].kind == Value::Text && known(leaf->ref))
                    out->emplace_back(at + "." + x.keys[k], x.list[k].t);
    }
}

bool has_refs(const Schema& schema, const FieldSpec& f) {
    auto known = [&](const std::string& reg) { return !reg.empty() && schema.registry(reg); };
    if (f.type == FieldType::Id || (f.type == FieldType::List && f.element == FieldType::Id)) return known(f.ref);
    if (f.type == FieldType::List && f.element == FieldType::Struct)
        for (const FieldSpec& leaf : f.element_fields)
            if (leaf.type == FieldType::Id && known(leaf.ref)) return true;
    return false;
}

// The registry a reference at path points into (the field's, or its element leaf's).
std::string ref_registry(const FieldSpec& f, const std::string& path) {
    if (f.type != FieldType::List || f.element != FieldType::Struct) return f.ref;
    const size_t dot = path.rfind('.');
    const FieldSpec* leaf = dot == std::string::npos ? nullptr : f.element_field(path.substr(dot + 1));
    return leaf ? leaf->ref : std::string();
}

// "" when the entry is there; else why not, as a reference to it says it.
std::string why_gone(const Ctx& c, const State& s, const std::string& reg, const std::string& id, const std::map<EntryRef, std::string>& cascaded) {
    const std::string key = lower(id);
    const EntryState* e = s.entry(reg, key);
    const auto refused = c.refused_add.find({reg, key});
    if (!e) return refused != c.refused_add.end() ? "refused: " + refused->second : "does not exist";
    if (!e->removed) return "";
    if (e->retired) return "is retired";
    if (!e->removed_in.empty()) return "was removed by game build " + e->removed_in;
    if (refused != c.refused_add.end()) return "refused: " + refused->second;
    if (const auto it = cascaded.find({reg, key}); it != cascaded.end()) return "was removed: " + it->second;
    return "was removed by " + e->removed_by;
}

// One chain row over value v (its field's spec f). false + *why: it no longer applies there.
bool apply_row(const ChainRow& r, const Value& v, const FieldSpec& f, Value* out, std::string* why) {
    if (r.element.on) return r.element_spec && element_step(r.element, r.op, *r.element_spec, v, out, why);
    if (r.list != ListOp::None) return *out = list_step(r.list, r.list_values, v, &f), true;
    if (!op_result(r.op, v, f, out, why)) return false;
    return true;
}

void check_references(Ctx& c, State& s) {
    std::map<EntryRef, std::string> cascaded;  // game entries removed with what they name: why
    std::vector<std::pair<std::string, const FieldSpec*>> fields;  // (registry, field) with references
    for (const auto& [name, reg] : c.schema.registries)
        for (const auto& [fname, f] : reg.fields)
            if (fname.find("[]") == std::string::npos && has_refs(c.schema, f)) fields.emplace_back(name, &f);  // a leaf row is checked through its list
    if (fields.empty()) return;
    auto first_bad = [&](const FieldSpec& f, const Value& v, std::string* text) {
        std::vector<Ref> refs;
        refs_of(c.schema, f, v, f.name, &refs);
        for (const auto& [path, id] : refs) {
            const std::string why = why_gone(c, s, ref_registry(f, path), id, cascaded);
            if (!why.empty()) return *text = path + " " + id + " " + why, true;
        }
        return false;
    };
    for (int round = 0; round < 64; ++round) {
        bool changed = false;
        for (const auto& [reg, f] : fields) {
            std::vector<std::string> ids;
            s.each_entry(reg, [&](const EntryState& e) {
                if (!e.removed) ids.push_back(e.id);
            });
            for (const std::string& id : ids) {
                EntryState* e = s.entry(reg, id);
                if (!e || e->removed) continue;
                const FieldRef fr{reg, id, f->name};
                std::string bad;
                if (!first_bad(*f, s.current(fr), &bad)) continue;
                const auto adder = c.adders.find({reg, id});
                // 1: the registry edits that brought a bad reference in are taken out, here only (the entry's own add stands)
                if (auto it = s.fields.find(fr); it != s.fields.end()) {
                    FieldOutcome& fo = it->second;
                    std::vector<ChainRow> kept;
                    Value v = fo.base;
                    std::string had;
                    bool dropped = false;
                    for (ChainRow& r : fo.chain) {
                        const bool own_add = adder != c.adders.end() && r.kind == WriterKind::Registry && r.stage == Stage::Add && r.mod == adder->second.mod;
                        Value next;
                        std::string why;
                        if (r.kind != WriterKind::Registry || own_add) {
                            if (apply_row(r, v, *f, &next, &why)) v = next;
                            r.after = v;
                            kept.push_back(std::move(r));
                            continue;
                        }
                        const bool before = first_bad(*f, v, &had);
                        std::string now;
                        if (!apply_row(r, v, *f, &next, &why) || (!before && first_bad(*f, next, &now))) {
                            dropped = true;
                            c.out.refused.push_back({r.mod, r.source, r.stage, r.n,
                                                     reg + " " + id + ": " + (why.empty() ? now : why) + "; this edit was not applied there", ""});
                            continue;
                        }
                        v = next;
                        r.after = v;
                        kept.push_back(std::move(r));
                    }
                    if (dropped) {
                        changed = true;
                        fo.chain = std::move(kept);
                        fo.result = v;
                        fo.winner = fo.chain.empty() ? std::string() : fo.chain.back().who;
                        if (fo.chain.empty()) s.fields.erase(it);
                        if (!first_bad(*f, s.current(fr), &bad)) continue;
                    }
                }
                // 2: still naming what is not there: the entry goes, reported with the cause chain
                changed = true;
                e->removed = true;
                if (adder != c.adders.end()) {
                    c.refused_add[{reg, id}] = bad;
                    e->removed_by = "refused";
                    c.out.refused.push_back({adder->second.mod, adder->second.source, Stage::Add, adder->second.n, reg + " " + id + " refused: " + bad, reg + "|" + id});
                    std::erase_if(c.out.added, [&](const EntryRef& a) { return a.registry == reg && a.id == id; });
                    for (auto x = s.fields.lower_bound({reg, id, ""}); x != s.fields.end() && x->first.registry == reg && x->first.id == id;) x = s.fields.erase(x);
                } else {
                    cascaded[{reg, id}] = bad;
                    e->removed_by = "the reference check";
                    c.out.removed.push_back({reg, id});
                    c.out.lines.push_back({LineKind::Dropped, fr, "", "", reg + " " + id + " removed: " + bad});
                }
            }
        }
        if (!changed) break;
    }
}

}  // namespace

Result compose(const Schema& schema, const BaseModel& base, const std::vector<OldWrite>& old, const std::vector<ModInput>& mods) {
    Result out;
    Ctx c{schema, out, base, {}, {}, {}};
    for (const auto& [reg, covers] : base.raw_cover)
        for (const auto& [key, cover] : covers)
            if (cover.converted) c.converted[{reg, cover.field}].push_back(key);
    for (const ModInput& m : mods)  // refused before the stage: a reference to such an add names this as its cause
        for (const Refusal& x : m.refused)
            if (const size_t bar = x.entry.find('|'); bar != std::string::npos)
                c.refused_add.try_emplace({x.entry.substr(0, bar), lower(x.entry.substr(bar + 1))}, x.reason);
    State s;
    for (const auto& [reg, entries] : base.registries)
        for (const BaseModel::Entry& e : entries) {
            EntryState x;
            x.id = e.id;
            x.traits = e.traits;
            x.base = e.fields;
            x.removed_in = e.removed_in;
            x.removed = !e.removed_in.empty();
            x.removed_by = e.removed_in.empty() ? "" : "game build " + e.removed_in;
            s.add_entry(reg, std::move(x));
        }
    // retired ids ("<registry>|<id>") hold their place, removed: an add of one is refused as taken, an edit as missing
    for (const std::string& rid : base.retired) {
        const size_t bar = rid.find('|');
        if (bar == std::string::npos) continue;
        const std::string reg = rid.substr(0, bar), id = rid.substr(bar + 1);
        if (s.entry(reg, id)) continue;
        EntryState x;
        x.id = id;
        x.removed = x.retired = true;
        x.removed_by = "the ids ledger (retired)";
        s.add_entry(reg, std::move(x));
    }
    // D1: Kiln's older paths and the archive writer's packages lie below every registry edit, in the order they apply
    // (a raw member's chain too: "raw:<key>", when the member resolves on this build). Old data packs (and Content.* code)
    // are applied by the game's kiln.content as today: each one's relative op over the value below the packs (what the game
    // loaded: the archive writer's and old boot patches' result), the later one winning (packs.cpp parse_value); so two
    // packs' mul 2 and mul 3 give x3, not x6. Registry edits above them compose on the result.
    std::map<FieldRef, Value> below_packs;
    for (const OldWrite& w : old) {
        const RegistrySpec* reg = schema.registry(w.field.registry);
        if (!reg || !s.entry(w.field.registry, w.field.id)) continue;
        const FieldSpec* f = reg->field(w.field.field);
        const RawMember* m = nullptr;
        if (!f && is_raw_field(w.field.field)) {
            std::string why;
            if ((m = s.raw_member(base, w.field.registry, w.field.id, w.field.field.substr(4), &why))) f = &m->spec;
        }
        if (!f) continue;
        FieldOutcome& fo = s.touch(w.field);
        if (m) {
            if (fo.raw.empty()) fo.raw = m->spec.name;
            if (!fo.raw_spec) fo.raw_spec = m->spec;
        }
        Value from = fo.result;
        if (w.kind == WriterKind::OldPack || w.kind == WriterKind::Code) from = below_packs.try_emplace(w.field, fo.result).first->second;
        ChainRow r;
        r.kind = w.kind;
        r.mod = w.mod;
        r.who = w.who;
        r.op = w.op;
        r.list = w.list;
        r.list_values = w.list_values;
        r.after = w.list == ListOp::None ? apply_op(w.op, from, *f) : list_step(w.list, w.list_values, from, f);
        fo.result = r.after;
        fo.winner = w.who;
        fo.chain.push_back(std::move(r));
    }
    for (const Stage stage : {Stage::Add, Stage::Alter})
        for (const ModInput& m : mods) run_mod(c, s, m, stage);
    // final: each mod on its own overlay over the post-alter state (it sees add+alter and its own final edits, never another
    // mod's; the post-alter state is not changed until every final ran); the entries it named are composed afterwards, in
    // load order, with alter's rules
    struct Held {
        Who who;
        Edit edit;
        std::vector<std::string> ids;
        std::string dropped_by;
    };
    std::vector<Held> held;
    for (const ModInput& m : mods) {
        const bool any = m.code || std::any_of(m.batches.begin(), m.batches.end(), [](const Batch& b) { return b.stage == Stage::Final; });
        if (!any) continue;
        std::string bad_id;
        if (!mod_id_ok(m.id, &bad_id)) {
            State scratch;
            scratch.parent = &s;
            run_mod(c, scratch, m, Stage::Final);  // refuses each of its edits
            continue;
        }
        State scratch;
        scratch.parent = &s;
        Result ignored;
        Ctx sc{schema, ignored, base, c.converted, {}, {}};
        auto take = [&](const Who& w, const Edit& e, const std::string& batch_mod) {
            if (batch_mod != m.id) return refuse(c, w, e, "a batch of mod " + batch_mod + " in " + m.id + "'s place");
            if (e.entry == EntryOp::Add) return refuse(c, w, e, "an entry is added in stage add");
            const RegistrySpec* reg = schema.registry(e.registry);
            if (!reg) return refuse(c, w, e, "no registry \"" + e.registry + "\"");
            if (e.entry != EntryOp::Remove && e.raw.empty() && !reg->field(e.element.on ? e.element.list : e.field)) return refuse(c, w, e, e.registry + " has no field \"" + e.field + "\"");
            std::vector<std::string> ids;
            std::string why, dropped_by;
            if (!select(StateView(scratch), schema, scratch, e.registry, e.select, &ids, &why, &dropped_by)) return refuse(c, w, e, why);
            held.push_back({w, e, ids, dropped_by});
            if (dropped_by.empty()) run_edit(sc, scratch, w, e, false, &ids);  // its own later reads see it
        };
        for (const Batch& b : m.batches)
            if (b.stage == Stage::Final)
                for (const Edit& e : b.edits) take({m.id, b.source, Stage::Final}, e, b.mod);
        if (m.code) {
            Batch cb;
            cb.mod = m.id;
            cb.stage = Stage::Final;
            cb.source = "code";
            m.code(Stage::Final, StateView(scratch), &cb);
            for (const Edit& e : cb.edits) take({m.id, cb.source, Stage::Final}, e, cb.mod);
        }
    }
    for (const Held& h : held) {
        if (!h.dropped_by.empty()) {
            out.lines.push_back({LineKind::Dropped, {h.edit.registry, h.edit.select.id, edit_field(h.edit)}, h.dropped_by, h.who.mod,
                                 "dropped: " + h.edit.select.id + " was removed by " + h.dropped_by + "; " + h.who.mod + "'s edit into it is not applied"});
            continue;
        }
        run_edit(c, s, h.who, h.edit, true, &h.ids);
    }
    check_references(c, s);
    out.fields = std::move(s.fields);
    return out;
}

}  // namespace kc::registry
