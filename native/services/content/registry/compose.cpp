// SPDX-License-Identifier: MIT
// kiln.content registry stage: composition and conflicts (compose.h).
#include "compose.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace kc::reg {

const char* writer_kind_name(WriterKind k) {
    switch (k) {
        case WriterKind::ArchiveWriter: return "archiveWriter";
        case WriterKind::OldPatch: return "oldPatch";
        case WriterKind::OldPack: return "oldPack";
        case WriterKind::Code: return "code";
        default: return "registry";
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

Value evaluate(const std::vector<ChainRow>& rows, const Value& below, const FieldSpec& f) {
    Value v = below;
    for (const ChainRow& r : rows) {
        if (r.list == ListOp::None) {
            v = apply_op(r.op, v, f);
            continue;
        }
        std::vector<Value> l = v.kind == Value::List ? v.list : std::vector<Value>{};
        if (r.list == ListOp::Clear) l.clear();
        else if (r.list == ListOp::Replace) l = r.list_values;
        else if (r.list == ListOp::Append) l.insert(l.end(), r.list_values.begin(), r.list_values.end());
        else std::erase_if(l, [&](const Value& x) { return std::find(r.list_values.begin(), r.list_values.end(), x) != r.list_values.end(); });
        v = Value::of_list(std::move(l));
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
};

struct State {
    std::map<std::string, std::vector<EntryState>> regs;
    std::map<EntryRef, size_t> at;
    std::map<FieldRef, FieldOutcome> fields;

    const EntryState* entry(const std::string& reg, const std::string& id) const {
        const auto it = at.find({reg, id});
        return it == at.end() ? nullptr : &regs.at(reg)[it->second];
    }
    EntryState* entry(const std::string& reg, const std::string& id) {
        const auto it = at.find({reg, id});
        return it == at.end() ? nullptr : &regs[reg][it->second];
    }
    Value base_of(const FieldRef& f) const {
        const EntryState* e = entry(f.registry, f.id);
        if (!e) return {};
        const auto b = e->base.find(f.field);
        return b == e->base.end() ? Value{} : b->second;
    }
    Value current(const FieldRef& f) const {
        const auto it = fields.find(f);
        return it != fields.end() ? it->second.result : base_of(f);
    }
    FieldOutcome& touch(const FieldRef& f) {
        auto [it, fresh] = fields.try_emplace(f);
        if (fresh) it->second.result = it->second.base = base_of(f);
        return it->second;
    }
    void add_entry(const std::string& reg, EntryState e) {
        auto& list = regs[reg];
        at[{reg, e.id}] = list.size();
        list.push_back(std::move(e));
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
        const auto it = s_.regs.find(reg);
        if (it != s_.regs.end())
            for (const EntryState& e : it->second)
                if (!e.removed) out.push_back(e.id);
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
    const std::string id = sel.id.find(':') == std::string::npos && sel.id != "*" ? "game:" + lower(sel.id) : sel.id;
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
            if (e && e->removed && e->removed_by != "retired") return *dropped_by = e->removed_by, true;
            return *why = "no entry " + id + " in " + reg, false;
        }
        if (keep(id)) ids->push_back(id);
        return true;
    }
    for (const std::string& e : v.ids(reg))
        if (glob_ci(id, e) && keep(e)) ids->push_back(e);
    return true;
}

bool absolute(const Edit& e) { return e.list == ListOp::None ? e.op.set : (e.list == ListOp::Clear || e.list == ListOp::Replace); }
bool absolute(const ChainRow& r) { return r.list == ListOp::None ? r.op.set : (r.list == ListOp::Clear || r.list == ListOp::Replace); }

// The value an edit makes of cur. false: refused (*why).
bool next_value(const Edit& e, const Value& cur, const FieldSpec& f, Value* next, std::string* why) {
    if (e.list == ListOp::None) {
        if (e.op.relative() && !cur.numeric()) return *why = f.name + " has no number to change here", false;
        *next = apply_op(e.op, cur, f);
        if (next->numeric() && ((f.min && next->as_double() < *f.min) || (f.max && next->as_double() > *f.max)))
            return *why = f.name + ": the result " + next->text_of() + " is outside the field's range", false;
        return true;
    }
    ChainRow r;
    r.list = e.list;
    r.list_values = e.list_values;
    *next = evaluate({r}, cur, f);
    if (f.max_items && next->list.size() > f.max_items)
        return *why = f.name + " would hold " + std::to_string(next->list.size()) + " values; it holds at most " + std::to_string(f.max_items), false;
    return true;
}

struct Who {
    std::string mod, source;
    Stage stage;
};

// One field edit over its entries: all or nothing. report: false while a mod's final runs on its own scratch copy.
bool apply_field_edit(Ctx& c, State& s, const Who& w, const Edit& e, const std::vector<std::string>& ids, bool report, std::string* why) {
    const FieldSpec& f = *c.schema.registry(e.registry)->field(e.field);
    struct Step {
        FieldRef ref;
        Value next;
        bool yielded = false, line = false;
        LineKind kind = LineKind::Overridden;
        std::string other;
    };
    std::vector<Step> steps;
    for (const std::string& id : ids) {
        Step st;
        st.ref = {e.registry, id, e.field};
        const auto fo = s.fields.find(st.ref);
        const Value cur = s.current(st.ref);
        // another writer: another mod's registry edit, an old path of another mod, an archive-writer package (the mod's own
        // older path is no conflict with itself)
        auto other = [&](const ChainRow& r) { return r.mod != w.mod || r.mod.empty(); };
        const ChainRow* last_other = nullptr;  // the latest other writer
        const ChainRow* prev = nullptr;        // the row just before this edit
        bool other_exclusive = false;
        if (fo != s.fields.end())
            for (const ChainRow& r : fo->second.chain) {
                prev = &r;
                if (!other(r)) continue;
                last_other = &r;
                other_exclusive = other_exclusive || r.exclusive;
            }
        if (e.yield && fo != s.fields.end() && !fo->second.chain.empty()) {
            st.yielded = true;
            steps.push_back(std::move(st));
            continue;
        }
        if (!next_value(e, cur, f, &st.next, why)) return *why = id + ": " + *why, false;
        if (last_other) {
            const bool agreed = absolute(e) && absolute(*last_other) && st.next == cur;
            if ((e.exclusive || other_exclusive) && !agreed)
                return *why = "Conflict (exclusive): " + last_other->who + " also writes " + st.ref.text() + "; the later edit is refused", false;
        }
        if (prev && other(*prev)) {  // a line when this edit follows another writer's (not after the mod's own)
            if (absolute(e)) {
                st.line = !(st.next == cur);
                st.kind = LineKind::Overridden;
            } else {
                st.line = true;
                st.kind = absolute(*prev) ? LineKind::ComposedOn : LineKind::Composed;
            }
            st.other = prev->who;
        }
        steps.push_back(std::move(st));
    }
    for (Step& st : steps) {
        if (st.yielded) {
            if (report)
                c.out.lines.push_back({LineKind::Yielded, st.ref, "", w.mod, "yielded: " + st.ref.text() + " was set by an earlier writer; " + w.mod + "'s edit is not applied there"});
            continue;
        }
        FieldOutcome& fo = s.touch(st.ref);
        const Value before = fo.result;
        ChainRow r;
        r.kind = WriterKind::Registry;
        r.mod = r.who = w.mod;
        r.stage = w.stage;
        r.source = w.source;
        r.n = e.n;
        r.op = e.op;
        r.list = e.list;
        r.list_values = e.list_values;
        r.exclusive = e.exclusive;
        r.after = st.next;
        fo.chain.push_back(std::move(r));
        fo.result = st.next;
        fo.winner = w.mod;
        if (report && st.line) {
            std::string text = st.kind == LineKind::Overridden
                                   ? st.ref.text() + ": " + w.mod + " sets " + st.next.text_of() + " over " + st.other + "'s " + before.text_of() + " (the later one wins)"
                               : st.kind == LineKind::ComposedOn
                                   ? st.ref.text() + ": " + w.mod + "'s change composed on " + st.other + "'s value: " + before.text_of() + " -> " + st.next.text_of()
                                   : st.ref.text() + ": " + w.mod + "'s change stacks on " + st.other + "'s: " + before.text_of() + " -> " + st.next.text_of();
            c.out.lines.push_back({st.kind, st.ref, w.mod, st.other, std::move(text)});
        }
    }
    return true;
}

void refuse(Ctx& c, const Who& w, const Edit& e, const std::string& why) { c.out.refused.push_back({w.mod, w.source, w.stage, e.n, why}); }

// One edit at its mod's turn on state s. resolved: the entries a final edit named on its mod's own copy (else resolved here).
void run_edit(Ctx& c, State& s, const Who& w, const Edit& e, bool report, const std::vector<std::string>* resolved) {
    const RegistrySpec* reg = c.schema.registry(e.registry);
    if (!reg) return refuse(c, w, e, "no registry \"" + e.registry + "\"");
    if (e.entry == EntryOp::Add) {
        if (w.stage != Stage::Add) return refuse(c, w, e, "an entry is added in stage add");
        if (e.add_id.rfind(w.mod + ":", 0) != 0) return refuse(c, w, e, "an added entry's id is \"" + w.mod + ":<name>\"");
        if (s.entry(e.registry, e.add_id)) return refuse(c, w, e, "the id " + e.add_id + " is taken (ids are never reused)");
        EntryState n;
        n.id = e.add_id;
        if (!e.like.empty()) {
            const EntryState* l = s.entry(e.registry, e.like);
            if (!l || l->removed) return refuse(c, w, e, "like: no entry " + e.like + " in " + e.registry);
            n.traits = l->traits;
            n.base = l->base;
            for (const auto& [ref, fo] : s.fields)
                if (ref.registry == e.registry && ref.id == e.like) n.base[ref.field] = fo.result;
        }
        s.add_entry(e.registry, std::move(n));
        if (report) c.out.added.push_back({e.registry, e.add_id});
        for (const auto& [k, v] : e.add_fields) {
            Edit set;
            set.n = e.n;
            set.registry = e.registry;
            set.field = k;
            set.op.set = true;
            set.op.value = v;
            std::string why;
            if (!reg->field(k)) refuse(c, w, e, e.registry + " has no field \"" + k + "\"");
            else if (!apply_field_edit(c, s, w, set, {e.add_id}, report, &why)) refuse(c, w, e, why);
        }
        return;
    }
    if (w.stage == Stage::Add) return refuse(c, w, e, "stage add adds entries; field edits belong in alter or final");
    if (e.entry != EntryOp::Remove && !reg->field(e.field)) return refuse(c, w, e, e.registry + " has no field \"" + e.field + "\"");
    std::vector<std::string> ids;
    if (resolved) {  // a final edit: entries another mod's final removed since are dropped
        for (const std::string& id : *resolved) {
            const EntryState* x = s.entry(e.registry, id);
            if (x && !x->removed) ids.push_back(id);
            else if (x && report)
                c.out.lines.push_back({LineKind::Dropped, {e.registry, id, e.field}, x->removed_by, w.mod,
                                       "dropped: " + id + " was removed by " + x->removed_by + "; " + w.mod + "'s edit into it is not applied"});
        }
    } else {
        std::string why, dropped_by;
        if (!select(StateView(s), c.schema, s, e.registry, e.select, &ids, &why, &dropped_by)) return refuse(c, w, e, why);
        if (!dropped_by.empty()) {
            if (report)
                c.out.lines.push_back({LineKind::Dropped, {e.registry, e.select.id, e.field}, dropped_by, w.mod,
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
    std::string why;
    if (!apply_field_edit(c, s, w, e, ids, report, &why)) refuse(c, w, e, why);
}

void run_mod(Ctx& c, State& s, const ModInput& m, Stage stage) {
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

Result compose(const Schema& schema, const BaseModel& base, const std::vector<OldWrite>& old, const std::vector<ModInput>& mods) {
    Result out;
    Ctx c{schema, out};
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
        x.removed = true;
        x.removed_by = "retired";
        s.add_entry(reg, std::move(x));
    }
    // D1: Kiln's older paths and the archive writer's packages lie below every registry edit, in the order they apply
    for (const OldWrite& w : old) {
        const RegistrySpec* reg = schema.registry(w.field.registry);
        const FieldSpec* f = reg ? reg->field(w.field.field) : nullptr;
        if (!f || !s.entry(w.field.registry, w.field.id)) continue;
        FieldOutcome& fo = s.touch(w.field);
        ChainRow r;
        r.kind = w.kind;
        r.mod = w.mod;
        r.who = w.who;
        r.op = w.op;
        r.after = apply_op(w.op, fo.result, *f);
        fo.result = r.after;
        fo.winner = w.who;
        fo.chain.push_back(std::move(r));
    }
    for (const Stage stage : {Stage::Add, Stage::Alter})
        for (const ModInput& m : mods) run_mod(c, s, m, stage);
    // final: each mod on its own copy of the post-alter state (it sees add+alter and its own final edits, never another
    // mod's); the entries it named are composed afterwards, in load order, with alter's rules
    struct Held {
        Who who;
        Edit edit;
        std::vector<std::string> ids;
        std::string dropped_by;
    };
    std::vector<Held> held;
    const State frozen = s;
    for (const ModInput& m : mods) {
        const bool any = m.code || std::any_of(m.batches.begin(), m.batches.end(), [](const Batch& b) { return b.stage == Stage::Final; });
        if (!any) continue;
        State scratch = frozen;
        Result ignored;
        Ctx sc{schema, ignored};
        auto take = [&](const Who& w, const Edit& e, const std::string& batch_mod) {
            if (batch_mod != m.id) return refuse(c, w, e, "a batch of mod " + batch_mod + " in " + m.id + "'s place");
            if (e.entry == EntryOp::Add) return refuse(c, w, e, "an entry is added in stage add");
            const RegistrySpec* reg = schema.registry(e.registry);
            if (!reg) return refuse(c, w, e, "no registry \"" + e.registry + "\"");
            if (e.entry != EntryOp::Remove && !reg->field(e.field)) return refuse(c, w, e, e.registry + " has no field \"" + e.field + "\"");
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
            out.lines.push_back({LineKind::Dropped, {h.edit.registry, h.edit.select.id, h.edit.field}, h.dropped_by, h.who.mod,
                                 "dropped: " + h.edit.select.id + " was removed by " + h.dropped_by + "; " + h.who.mod + "'s edit into it is not applied"});
            continue;
        }
        run_edit(c, s, h.who, h.edit, true, &h.ids);
    }
    out.fields = std::move(s.fields);
    return out;
}

}  // namespace kc::reg
