// SPDX-License-Identifier: MIT
// kiln.content registry stage: the stage runner (stage.h).
#include "stage.h"

#include <algorithm>
#include <cstdio>

#include "../../../core/src/services/boot_gamedata.h"
#include "schema_ir.h"

namespace kc {  // content.h's (packs.cpp), declared here
std::string guid_text(const uint8_t g[16]);
bool guid_parse(const std::string& s, uint8_t out[16]);
}  // namespace kc

namespace kc::registry::stage {

namespace {

std::string lc(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string sha_hex(const std::string& s) { return kiln::boot_gd::hex_of(kiln::boot_gd::sha256(s)); }

const char* applies_word(Applies a) { return a == Applies::Now ? "now" : a == Applies::WorldLoad ? "worldLoad" : "restart"; }

// ---- the decoded base in the cache: values with their kinds kept ----
json value_out(const Value& v) {
    switch (v.kind) {
        case Value::Int: return {{"i", v.i}};
        case Value::Num: return {{"d", v.d}};
        case Value::Bool: return {{"b", v.b}};
        case Value::Text: return {{"t", v.t}};
        case Value::List: {
            json a = json::array();
            for (const Value& x : v.list) a.push_back(value_out(x));
            return {{"l", a}};
        }
        case Value::Struct: {
            json a = json::array();
            for (const Value& x : v.list) a.push_back(value_out(x));
            return {{"k", v.keys}, {"s", a}};
        }
        default: return nullptr;
    }
}
bool value_in(const json& j, Value* v) {
    *v = Value{};
    if (j.is_null()) return true;
    if (!j.is_object()) return false;
    if (j.contains("i") && j["i"].is_number_integer()) return *v = Value::integer(j["i"].get<int64_t>()), true;
    if (j.contains("d") && j["d"].is_number()) return *v = Value::number(j["d"].get<double>()), true;
    if (j.contains("b") && j["b"].is_boolean()) return *v = Value::boolean(j["b"].get<bool>()), true;
    if (j.contains("t") && j["t"].is_string()) return *v = Value::text(j["t"].get<std::string>()), true;
    if (j.contains("l") && j["l"].is_array()) {
        std::vector<Value> out;
        for (const auto& x : j["l"]) {
            Value e;
            if (!value_in(x, &e)) return false;
            out.push_back(std::move(e));
        }
        return *v = Value::of_list(std::move(out)), true;
    }
    if (j.contains("k") && j.contains("s") && j["k"].is_array() && j["s"].is_array() && j["k"].size() == j["s"].size()) {
        v->kind = Value::Struct;
        for (size_t i = 0; i < j["k"].size(); ++i) {
            Value e;
            if (!j["k"][i].is_string() || !value_in(j["s"][i], &e)) return false;
            v->keys.push_back(j["k"][i].get<std::string>());
            v->list.push_back(std::move(e));
        }
        return true;
    }
    return false;
}

json base_out(const BaseModel& b) {
    json regs = json::object();
    for (const auto& [reg, es] : b.registries) {
        json a = json::array();
        for (const BaseModel::Entry& e : es) {
            json f = json::object();
            for (const auto& [k, v] : e.fields) f[k] = value_out(v);
            json row = {{"id", e.id}, {"fields", f}};
            if (!e.traits.empty()) row["traits"] = e.traits;
            if (!e.removed_in.empty()) row["removedIn"] = e.removed_in;
            a.push_back(std::move(row));
        }
        regs[reg] = std::move(a);
    }
    json cover = json::object();
    for (const auto& [reg, m] : b.raw_cover)
        for (const auto& [k, c] : m) cover[reg][k] = {{"field", c.field}, {"converted", c.converted}};
    return {{"registries", regs}, {"rawCover", cover}, {"retired", b.retired}};
}
bool base_in(const json& j, BaseModel* b) {
    if (!j.is_object() || !j.contains("registries") || !j["registries"].is_object()) return false;
    for (auto it = j["registries"].begin(); it != j["registries"].end(); ++it) {
        auto& dst = b->registries[it.key()];
        for (const auto& row : it.value()) {
            BaseModel::Entry e;
            if (!row.contains("id") || !row["id"].is_string()) return false;
            e.id = row["id"].get<std::string>();
            for (auto f = row["fields"].begin(); f != row["fields"].end(); ++f) {
                Value v;
                if (!value_in(f.value(), &v)) return false;
                e.fields[f.key()] = std::move(v);
            }
            if (row.contains("traits")) e.traits = row["traits"].get<std::set<std::string>>();
            if (row.contains("removedIn")) e.removed_in = row["removedIn"].get<std::string>();
            dst.push_back(std::move(e));
        }
    }
    if (j.contains("rawCover"))
        for (auto r = j["rawCover"].begin(); r != j["rawCover"].end(); ++r)
            for (auto k = r.value().begin(); k != r.value().end(); ++k)
                b->raw_cover[r.key()][k.key()] = {k.value().value("field", std::string()), k.value().value("converted", false)};
    if (j.contains("retired")) b->retired = j["retired"].get<std::set<std::string>>();
    return true;
}

json need_json(const model::Need& n) {
    return {{"all", n.all}, {"fields", n.fields}, {"whole", n.whole}, {"raw", n.raw}, {"traits", n.traits}, {"registries", n.registries}};
}

std::string key_text(const KilnAssetKey& k) {
    char b[32];
    std::snprintf(b, sizeof b, "/%08x/%u", static_cast<unsigned>(k.type), static_cast<unsigned>(k.part));
    return guid_text(k.guid) + b;
}
bool key_parse(const std::string& s, KilnAssetKey* k) {
    if (s.size() < 38 || s[36] != '/') return false;
    if (!guid_parse(s.substr(0, 36), k->guid)) return false;
    const size_t slash = s.find('/', 37);
    if (slash == std::string::npos) return false;
    k->type = static_cast<uint32_t>(std::stoul(s.substr(37, slash - 37), nullptr, 16));
    k->part = static_cast<uint32_t>(std::stoul(s.substr(slash + 1)));
    return true;
}

}  // namespace

// ---- inputs ----
void refuse_unmapped(const map::RegMap& rm, const Schema& schema, Batch* b, std::vector<Refusal>* refused) {
    std::vector<Edit> kept;
    for (Edit& e : b->edits) {
        std::string reason;
        auto unmapped = [&](const std::string& field) {
            if (field.empty() || is_raw_field(field)) return false;
            const RegistrySpec* rs = schema.registry(e.registry);
            const FieldSpec* f = rs ? rs->field(field) : nullptr;
            if (!f || f->read_only) return false;  // compose names an unknown or read-only field itself
            return rm.mapping(e.registry, generic_path(field), &reason) != map::Mapping::Mapped;
        };
        bool bad = false;
        // review finding 1 (ruling 2026-11-21): no back-end removes a game entry or creates a mod's in this release (P2 builds it):
        // every entry add and remove is refused here, before composition, so nothing is reported as done and nothing is removed
        // with it (a recipe needing a removed item stays); a reference to a refused add names this refusal
        if (e.entry != EntryOp::None) {
            bad = true;
            reason = e.registry + " " + (e.entry == EntryOp::Add ? "add " + e.add_id : "remove " + e.select.id) +
                     ": entry add and remove have no back-end in this release (no game entry is removed and no new entry is created); this edit was not applied";
        } else if (e.entry == EntryOp::Add) {
            for (const auto& [k, v] : e.add_fields) bad = bad || unmapped(k);
        } else if (e.raw.empty() && e.entry != EntryOp::Remove) {
            bad = unmapped(e.element.on ? (e.element.leaf.empty() ? e.element.list : e.element.list + "[]." + e.element.leaf) : e.field);
        }
        if (!bad) {
            kept.push_back(std::move(e));
            continue;
        }
        Refusal r{b->mod, b->source, b->stage, e.n, reason, ""};
        if (e.entry == EntryOp::Add) r.entry = e.registry + "|" + lc(e.add_id);
        refused->push_back(std::move(r));
    }
    b->edits = std::move(kept);
}

std::vector<Env::DataFile> read_sections(const std::string& mod, const std::string& version,
                                         const std::vector<std::pair<std::string, std::string>>& files, const Schema& schema) {
    std::vector<Env::DataFile> out;
    std::vector<std::pair<std::string, std::string>> sorted = files;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& [rel, text] : sorted) {
        if (text.find("\"registry\"") == std::string::npos) continue;
        const json j = json::parse(text, nullptr, false, true);
        if (!j.is_object() || !j.contains("registry")) continue;  // not JSON: the packs' reader says so at world load
        Env::DataFile f;
        f.mod = mod, f.version = version, f.rel = rel;
        if (!lower_section(j["registry"], mod, version, rel, schema, &f.batches, &f.refused, &f.error) && f.error.empty())
            f.error = "the registry section cannot be read";
        out.push_back(std::move(f));
    }
    return out;
}

// ---- the runner ----
struct Runner::Impl {
    Env env;
    mutable std::mutex mu;
    Schema schema = generated_schema();
    map::RegMap rm;
    std::vector<std::string> rm_errs;
    bool rm_loaded = false;
    std::vector<std::pair<std::string, std::string>> order;  // Kiln.Host's (set_order)
    bool order_set = false;
    struct Sub {
        uint64_t owner = 0;
        std::string mod;
        StageFn fn;
    };
    std::map<uint64_t, Sub> stage_subs;
    struct Ready {
        uint64_t owner = 0;
        std::function<void(Phase, uint64_t)> fn;
    };
    std::map<uint64_t, Ready> ready_subs;
    uint64_t next_handle = 1;
    std::map<std::string, std::vector<Batch>> queued;            // mod -> its submits before the stage
    std::map<std::string, std::vector<Refusal>> queued_refused;
    std::thread::id stage_thread;
    bool stage_thread_set = false;
    struct Turn {
        std::string mod;
        Stage stage = Stage::Alter;
        const View* view = nullptr;
        Batch* out = nullptr;
    };
    std::optional<Turn> turn;
    // the stage's result
    BaseModel base;
    Result result;
    std::vector<OldWrite> old;
    std::vector<ModInput> inputs;
    std::vector<Refusal> pre_refused;
    std::map<std::string, std::string> ids;
    std::map<std::string, uint32_t> nums;
    std::set<std::string> decoded;
    uint64_t generation = 0;
    Counts counts;
    std::string report;
    ReportMeta meta;
    std::vector<BootResource> boot;
    std::map<std::string, uint64_t> edits_by_mod;

    void log(int level, const std::string& line) {
        if (env.log) env.log(level, line);
    }
    bool load_regmap() {
        if (rm_loaded) return true;
        rm_loaded = true;
        if (!env.regmap || !env.regmap(&rm, &rm_errs)) {
            log(1, "registry: the regmap could not be read: every field is unproven on this build, edits of fields are refused");
            return false;
        }
        for (const std::string& e : rm_errs) log(1, "registry: regmap: " + e);
        return true;
    }
};

Runner::Runner(Env env) : d_(std::make_unique<Impl>()) { d_->env = std::move(env); }
Runner::~Runner() = default;

KilnResult Runner::set_order(const std::string& order_json) {
    const json j = json::parse(order_json, nullptr, false);
    if (!j.is_array()) return KILN_ERR_INVALID_ARGUMENT;
    std::vector<std::pair<std::string, std::string>> order;
    std::set<std::string> seen;
    for (const auto& row : j) {
        if (!row.is_object() || !row.contains("mod") || !row["mod"].is_string()) return KILN_ERR_INVALID_ARGUMENT;
        const std::string mod = row["mod"].get<std::string>();
        if (!seen.insert(mod).second) return KILN_ERR_INVALID_ARGUMENT;
        order.emplace_back(mod, row.contains("version") && row["version"].is_string() ? row["version"].get<std::string>() : std::string());
    }
    std::lock_guard lk(d_->mu);
    if (phase_.load() != Phase::None) return KILN_ERR_UNAVAILABLE;
    d_->order = std::move(order);
    d_->order_set = true;
    return KILN_OK;
}

KilnResult Runner::set_mode(uint32_t mode) {
    if (mode > 1) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lk(d_->mu);
    if (phase_.load() != Phase::None) return KILN_ERR_UNAVAILABLE;
    mode_.store(static_cast<Mode>(mode));
    return KILN_OK;
}

KilnResult Runner::subscribe_stage(uint64_t owner, const std::string& mod, StageFn fn, uint64_t* handle) {
    std::string err;
    if (!owner || !fn || !handle || !mod_id_ok(mod, &err)) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lk(d_->mu);
    if (phase_.load() != Phase::None) return KILN_ERR_UNAVAILABLE;
    for (const auto& [h, s] : d_->stage_subs)
        if (s.mod == mod) return KILN_ERR_INVALID_ARGUMENT;
    *handle = d_->next_handle++;
    d_->stage_subs[*handle] = {owner, mod, std::move(fn)};
    return KILN_OK;
}

KilnResult Runner::subscribe_ready(uint64_t owner, std::function<void(Phase, uint64_t)> fn, uint64_t* handle) {
    if (!owner || !fn || !handle) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lk(d_->mu);
    *handle = d_->next_handle++;
    d_->ready_subs[*handle] = {owner, std::move(fn)};
    return KILN_OK;
}

KilnResult Runner::unsubscribe(uint64_t handle) {
    std::lock_guard lk(d_->mu);
    if (d_->stage_subs.erase(handle) || d_->ready_subs.erase(handle)) return KILN_OK;
    return KILN_ERR_NOT_FOUND;
}

void Runner::owner_gone(uint64_t owner) {
    std::lock_guard lk(d_->mu);
    std::set<std::string> mods;
    for (auto it = d_->stage_subs.begin(); it != d_->stage_subs.end();)
        if (it->second.owner == owner) mods.insert(it->second.mod), it = d_->stage_subs.erase(it);
        else ++it;
    for (auto it = d_->ready_subs.begin(); it != d_->ready_subs.end();) it = it->second.owner == owner ? d_->ready_subs.erase(it) : std::next(it);
}

bool Runner::on_stage_thread() const {
    std::lock_guard lk(d_->mu);
    return d_->stage_thread_set && d_->stage_thread == std::this_thread::get_id();
}

KilnResult Runner::submit(uint64_t owner, const std::string& owner_mod, const std::string& ir, uint32_t* accepted, uint32_t* refused_n, std::string* report) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    Batch b;
    std::vector<Refusal> refused;
    std::string err;
    auto say = [&](KilnResult rc) {
        if (accepted) *accepted = static_cast<uint32_t>(b.edits.size());
        if (refused_n) *refused_n = static_cast<uint32_t>(refused.size());
        if (report) {
            json r = {{"accepted", b.edits.size()}, {"refused", json::array()}};
            for (const Refusal& x : refused) r["refused"].push_back({{"n", x.n}, {"reason", x.reason}});
            if (!err.empty()) r["error"] = err;
            *report = r.dump();
        }
        return rc;
    };
    {
        std::lock_guard lk(d_->mu);
        if (!parse_batch(ir, d_->schema, &b, &refused, &err)) return say(KILN_ERR_INVALID_ARGUMENT);
    }
    if (owner_mod.empty()) {  // review finding 8: Kiln binds every batch to its owner's mod; an owner it cannot bind has none
        err = "the caller's owner has no mod Kiln can bind it to (revoked, or not a mod's owner): the batch is refused";
        b.edits.clear();
        return say(KILN_ERR_INVALID_ARGUMENT);
    }
    if (lc(b.mod) != lc(owner_mod)) {
        err = "the batch names mod " + b.mod + ", not the caller's own mod " + owner_mod;
        b.edits.clear();
        return say(KILN_ERR_INVALID_ARGUMENT);
    }
    std::lock_guard lk(d_->mu);
    const Phase ph = phase_.load();
    if (ph == Phase::None) {
        if (b.source.empty()) b.source = "code:" + b.mod;
        d_->queued[b.mod].push_back(b);
        auto& q = d_->queued_refused[b.mod];
        q.insert(q.end(), refused.begin(), refused.end());
        return say(KILN_OK);
    }
    if (ph == Phase::Start || ph == Phase::WorldLoad) {
        err = "the stage is over: a change now is a live change";
        b.edits.clear();
        return say(KILN_ERR_UNAVAILABLE);
    }
    if (!(d_->stage_thread_set && d_->stage_thread == std::this_thread::get_id())) {
        err = "during the stage a batch is taken only on the stage thread";
        b.edits.clear();
        return say(KILN_ERR_WRONG_THREAD);
    }
    if (!d_->turn || d_->turn->mod != b.mod) {
        err = "during the stage a batch is taken only inside the stage callback of its mod";
        b.edits.clear();
        return say(d_->turn ? KILN_ERR_INVALID_ARGUMENT : KILN_ERR_UNAVAILABLE);
    }
    if (b.stage != d_->turn->stage) {
        err = std::string("the running stage is ") + stage_name(d_->turn->stage) + ", the batch's " + stage_name(b.stage);
        b.edits.clear();
        return say(KILN_ERR_INVALID_ARGUMENT);
    }
    refuse_unmapped(d_->rm, d_->schema, &b, &refused);
    Batch* out = d_->turn->out;
    if (out->source.empty()) out->source = "code:" + b.mod;
    for (Edit& e : b.edits) {
        e.n = static_cast<uint32_t>(out->edits.size());
        out->edits.push_back(e);
    }
    d_->pre_refused.insert(d_->pre_refused.end(), refused.begin(), refused.end());
    return say(KILN_OK);
}

std::vector<BootResource> Runner::run(const OldWritesFn& old_fn) {
    Impl& d = *d_;
    std::vector<ModInput> inputs;
    {
        std::lock_guard lk(d.mu);
        if (phase_.load() != Phase::None) return {};
        d.stage_thread = std::this_thread::get_id();
        d.stage_thread_set = true;
        phase_.store(Phase::Add);
    }
    d.load_regmap();
    if (mode_.load() == Mode::OnBoot)
        d.log(1, "registry: stage mode onBoot was asked (option A); this build runs the stage at resources-ready: a kiln-backend start's committed "
                 "view lists no resources and the boot snapshot is older than the start, so the base cannot be read before the entry point yet");
    // the order: Kiln.Host's runtime load order, else the mod folders'
    std::vector<std::pair<std::string, std::string>> order;
    {
        std::lock_guard lk(d.mu);
        order = d.order;
        d.meta.order_source = d.order_set ? "kiln.host" : "folders";
    }
    if (!d.meta.order_source.compare("folders") && d.env.folders) order = d.env.folders();
    std::vector<Env::DataFile> files = d.env.data ? d.env.data(d.schema) : std::vector<Env::DataFile>();
    std::map<std::string, ModInput> by_mod;
    std::map<std::string, std::string> versions;
    for (const auto& [m, v] : order) versions[m] = v;
    for (Env::DataFile& f : files) {
        ModInput& in = by_mod[f.mod];
        in.id = f.mod;
        if (versions[f.mod].empty()) versions[f.mod] = f.version;
        if (!f.error.empty()) {
            in.refused.push_back({f.mod, f.rel, Stage::Alter, 0, f.rel + ": " + f.error + "; nothing of this file's registry section was applied", ""});
            d.log(1, "registry: mod " + f.mod + ": " + f.rel + ": " + f.error + "; nothing of its registry section was applied");
        }
        for (Batch& b : f.batches) in.batches.push_back(std::move(b));
        in.refused.insert(in.refused.end(), f.refused.begin(), f.refused.end());
    }
    {
        std::lock_guard lk(d.mu);
        for (auto& [mod, bs] : d.queued) {
            ModInput& in = by_mod[mod];
            in.id = mod;
            for (Batch& b : bs) in.batches.push_back(std::move(b));
        }
        for (auto& [mod, rs] : d.queued_refused) by_mod[mod].refused.insert(by_mod[mod].refused.end(), rs.begin(), rs.end());
        d.queued.clear(), d.queued_refused.clear();
        for (const auto& [h, s] : d.stage_subs) {
            ModInput& in = by_mod[s.mod];
            in.id = s.mod;
            const std::string mod = s.mod;
            const uint64_t handle = h;
            in.code = [this, mod, handle](Stage st, const View& v, Batch* out) {
                StageFn fn;
                {
                    std::lock_guard l2(d_->mu);
                    const auto it = d_->stage_subs.find(handle);
                    if (it == d_->stage_subs.end()) return;
                    fn = it->second.fn;
                    d_->turn = Impl::Turn{mod, st, &v, out};
                    out->mod = mod;
                    out->stage = st;
                    phase_.store(st == Stage::Add ? Phase::Add : st == Stage::Alter ? Phase::Alter : Phase::Final);
                }
                const size_t before = out->edits.size();
                const bool clean = fn(st, v);
                std::lock_guard l2(d_->mu);
                d_->turn.reset();
                if (!clean) {
                    const size_t dropped = out->edits.size() - before;
                    out->edits.resize(before);
                    d_->stage_subs.erase(handle);
                    d_->pre_refused.push_back({mod, "code:" + mod, st, 0,
                                               "the stage callback faulted; its " + std::to_string(dropped) + " edit(s) of this stage were dropped and it is not called again", ""});
                    d_->log(2, "registry: mod " + mod + "'s stage callback faulted in " + stage_name(st) + ": its edits of that turn are dropped, the callback is removed");
                }
            };
        }
    }
    // the mapping check, then the inputs in the order (mods the order does not name after it, by id)
    std::vector<std::string> named;
    for (const auto& [m, v] : order)
        if (by_mod.count(m)) named.push_back(m);
    std::vector<std::string> rest;
    for (const auto& [m, in] : by_mod)
        if (std::find(named.begin(), named.end(), m) == named.end()) rest.push_back(m);
    if (!rest.empty() && d.meta.order_source == "kiln.host")
        for (const std::string& m : rest) d.log(0, "registry: mod " + m + " is not in Kiln.Host's load order: its edits run after every mod it names");
    named.insert(named.end(), rest.begin(), rest.end());
    for (const std::string& m : named) {
        ModInput in = std::move(by_mod[m]);
        for (Batch& b : in.batches) refuse_unmapped(d.rm, d.schema, &b, &in.refused);
        inputs.push_back(std::move(in));
    }
    // the base
    std::string why;
    // no mod has registry content: nothing is read (an empty registry.json says so), except once per game build, when the
    // running exe has no id table yet (work-bb 2026-11-21: kilnctl update needs one for every build)
    const std::string ids_rel = "ids-" + d.env.side + "-" + d.env.exe_sha256 + ".json";
    const bool ids_missing = d.env.read_data && d.env.write_data && !d.env.exe_sha256.empty() && !d.env.read_data(ids_rel);
    std::unique_ptr<model::Source> src = inputs.empty() && !ids_missing ? nullptr : d.env.source ? d.env.source(&why) : nullptr;
    d.meta.pid = d.env.pid;
    d.meta.kiln = d.env.kiln_version;
    d.meta.regmap = {d.env.side, d.env.exe_sha256, d.rm.has_build ? "regmap/" + d.env.side + "/" + d.env.exe_sha256 + ".json" : "", d.rm.has_build && d.rm.build.verified};
    d.meta.base.writer = d.env.writer;
    for (const ModInput& in : inputs) {
        json edits = json::array();
        for (const Batch& b : in.batches) {
            json eb = {{"stage", stage_name(b.stage)}, {"source", b.source}, {"n", b.edits.size()}};
            edits.push_back(eb);
        }
        d.meta.order.push_back({in.id, versions[in.id], sha_hex(edits.dump()).substr(0, 16)});
    }
    BaseModel base;
    std::vector<OldWrite> old;
    std::unique_ptr<model::Model> m;
    const model::Need need = model::needs_of(d.schema, inputs);
    bool hit = false;
    if (!src && inputs.empty()) {
        d.meta.base.origin = "none";
    } else if (inputs.empty()) {
        // only the id table of this build
        model::Model ids_model(*src, d.rm, d.schema);
        for (const auto& [n, r] : d.schema.registries) {
            std::string w;
            ids_model.entries(n, &w);
        }
        ids_model.id_table(&d.ids, &d.nums);
        d.env.write_data(ids_rel, ids_model.id_table_json().dump());
        d.log(0, "registry: the id table of this game build (" + d.env.side + " " + d.env.exe_sha256.substr(0, 8) + ") is written to the data folder (" +
                     std::to_string(d.ids.size()) + " entries); no mod has registry content");
        for (const std::string& n : ids_model.take_notes()) d.log(0, "registry: " + n);
        ids_model.release();
        src.reset();
        d.meta.base.origin = "none";
    } else if (!src) {
        d.log(2, "registry: the stage has no base to read (" + why + "): nothing of the registry is applied in this start");
        std::lock_guard lk(d.mu);
        d.counts.failed = true;
    } else {
        m = std::make_unique<model::Model>(*src, d.rm, d.schema);
        d.meta.base.origin = src->name();
        const std::string family = sha_hex(std::string(model::kDecoderVersion) + "\n" + d.env.kiln_version + "\n" + d.env.side + "\n" + d.env.exe_sha256 + "\n" +
                                           (d.rm.has_build ? (d.rm.build.verified ? "verified" : "unverified") : "none") + "\n" + need_json(need).dump());
        const std::string cache_rel = "cache/" + family.substr(0, 32) + ".json";
        // the cache: the same keys read again give the same digest -> the same base
        if (!need.all && need.raw.empty() && d.env.read_data)
            if (auto text = d.env.read_data(cache_rel)) {
                const json c = json::parse(*text, nullptr, false);
                bool ok = c.is_object() && c.value("family", std::string()) == family && c.contains("keys") && c["keys"].is_array();
                for (const auto& k : ok ? c["keys"] : json::array()) {
                    KilnAssetKey key{};
                    std::string w;
                    if (!k.is_array() || k.size() != 2 || !key_parse(k[0].get<std::string>(), &key) || !src->touch(key, k[1].get<std::string>(), &w)) {
                        ok = false;
                        break;
                    }
                }
                BaseModel cached;
                if (ok && src->digest() == c.value("digest", std::string()) && base_in(c["base"], &cached)) {
                    base = std::move(cached);
                    hit = true;
                    for (auto it = c["ids"].begin(); it != c["ids"].end(); ++it) d.ids[it.key()] = it.value().get<std::string>();
                    for (auto it = c["nums"].begin(); it != c["nums"].end(); ++it) d.nums[it.key()] = it.value().get<uint32_t>();
                    for (const auto& f : c["decoded"]) d.decoded.insert(f.get<std::string>());
                    d.log(0, "registry: the base of this start is the one the cache holds (" + std::to_string(c["keys"].size()) + " resource(s), digest " +
                                 c.value("digest", std::string()).substr(0, 16) + "): not decoded again");
                }
            }
        if (!hit) {
            m->fill(need, &base);
            // the id table of this build for kilnctl update: every R1 registry the first time this build starts with Kiln
            const bool want_all = ids_missing;
            if (want_all)
                for (const auto& [n, r] : d.schema.registries) {
                    std::string w;
                    m->entries(n, &w);
                }
            m->id_table(&d.ids, &d.nums);
            d.decoded = m->decoded();
            if (d.env.write_data && (want_all || !base.registries.empty())) d.env.write_data(ids_rel, m->id_table_json().dump());
            const std::string digest = m->digest();
            if (!need.all && need.raw.empty() && d.env.write_data && !digest.empty()) {
                json keys = json::array();
                for (const auto& [k, t] : m->keys_read()) keys.push_back({key_text(k), t});
                json ids = d.ids, nums = d.nums;
                d.env.write_data(cache_rel, json{{"format", 1}, {"family", family}, {"digest", digest}, {"keys", keys}, {"base", base_out(base)},
                                                 {"ids", ids}, {"nums", nums}, {"decoded", d.decoded}}
                                                .dump());
            }
        }
        if (old_fn) old = old_fn(*m);
        for (const std::string& n : m->take_notes()) d.log(0, "registry: " + n);
        if (!need.all) m->release();  // the base is decoded: let the bytes go (stage callbacks may read any field: after them)
    }
    Result r;
    if (src) {
        r = compose(d.schema, base, old, inputs);
        if (m) m->release();
    }
    // the edits refused before the stage are the report's too
    std::vector<Refusal> pre;
    for (const ModInput& in : inputs) pre.insert(pre.end(), in.refused.begin(), in.refused.end());
    {
        std::lock_guard lk(d.mu);
        pre.insert(pre.end(), d.pre_refused.begin(), d.pre_refused.end());
    }
    r.refused.insert(r.refused.begin(), pre.begin(), pre.end());
    // the boot back-end's resources: restart fields whose chains hold registry rows
    std::vector<BootResource> boot;
    for (const auto& [ref, fo] : r.fields) {
        const auto reg = d.rm.registries.find(ref.registry);
        if (reg == d.rm.registries.end() || is_raw_field(ref.field)) continue;
        const auto row = reg->second.rows.find(ref.field);
        if (row == reg->second.rows.end() || row->second.backend != map::Backend::Boot) continue;
        std::vector<ChainRow> rows;
        for (const ChainRow& c : fo.chain)
            if (c.kind == WriterKind::Registry) rows.push_back(c);
        if (rows.empty()) continue;
        const RegistrySpec* rs = d.schema.registry(ref.registry);
        const FieldSpec* f = rs ? rs->field(ref.field) : nullptr;
        const model::Entry* e = m ? m->find(ref.registry, ref.id) : nullptr;
        if (!f || !e) continue;
        auto it = std::find_if(boot.begin(), boot.end(), [&](const BootResource& b) { return std::memcmp(&b.key, &e->place.key, sizeof b.key) == 0; });
        if (it == boot.end()) {
            boot.push_back({e->place.key, e->place.type, {}});
            it = boot.end() - 1;
        }
        it->fields.push_back({ref, &reg->second, &row->second, *f, std::move(rows)});
    }
    std::lock_guard lk(d.mu);
    d.base = std::move(base);
    d.old = std::move(old);
    for (ModInput& in : inputs) in.code = nullptr;
    d.inputs = std::move(inputs);
    d.result = std::move(r);
    d.boot = boot;
    d.counts.refused = static_cast<uint32_t>(d.result.refused.size());
    d.counts.conflicts = static_cast<uint32_t>(d.result.lines.size());
    uint32_t edits = 0;
    for (const auto& [ref, fo] : d.result.fields)
        for (const ChainRow& c : fo.chain) edits += c.kind == WriterKind::Registry;
    d.counts.edits = edits + static_cast<uint32_t>(d.result.added.size());
    return boot;
}

void Runner::finish(const std::vector<BootOutcome>& outcomes) {
    Impl& d = *d_;
    std::vector<std::function<void(Phase, uint64_t)>> ready;
    LiveHandoff live;
    uint64_t gen = 0;
    {
        std::lock_guard lk(d.mu);
        if (phase_.load() == Phase::Start || phase_.load() == Phase::WorldLoad) return;
        for (const BootOutcome& o : outcomes) {
            auto fo = d.result.fields.find(o.field);
            if (fo == d.result.fields.end()) continue;
            if (o.ok) {
                if (!(fo->second.result == o.written))
                    d.log(1, "registry: " + o.field.text() + ": composed " + fo->second.result.text_of() + ", the resource holds " + o.written.text_of() +
                                 " after the old documents and the registry's rows (the report shows the resource's value)");
                fo->second.result = o.written;
                continue;
            }
            for (const ChainRow& c : fo->second.chain)
                if (c.kind == WriterKind::Registry)
                    d.result.refused.push_back({c.mod, c.source, c.stage, c.n, o.field.registry + " " + o.field.id + ": " + o.field.field + ": " + o.why + "; this edit was not applied", ""});
            d.log(1, "registry: " + o.field.text() + " was not written: " + o.why);
            d.result.fields.erase(fo);
        }
        d.counts.refused = static_cast<uint32_t>(d.result.refused.size());
        d.generation = 1;
        d.meta.generation = 1;
        d.meta.world_load = false;
        d.meta.written = d.env.now ? d.env.now() : "";
        // the plan (P1-PLAN section 6; P1 writes it for debugging and the compat dumps)
        json bootj = json::array();
        for (const BootResource& b : d.boot) {
            json fs = json::array();
            for (const BootField& f : b.fields) fs.push_back({{"field", f.field.text()}, {"rows", f.rows.size()}});
            bootj.push_back({{"resource", key_text(b.key)}, {"type", b.type}, {"fields", fs}});
        }
        json livej = json::array();
        for (const auto& [ref, fo] : d.result.fields) {
            const RegistrySpec* rs = d.schema.registry(ref.registry);
            const FieldSpec* f = rs && !is_raw_field(ref.field) ? rs->field(ref.field) : nullptr;
            if (f && f->applies == Applies::Restart) continue;
            livej.push_back({{"field", ref.text()}, {"applies", f ? applies_word(f->applies) : "now"}, {"result", fo.result.to_json()}});
        }
        json adds = json::array();
        for (const EntryRef& a : d.result.added) adds.push_back({{"registry", a.registry}, {"id", a.id}, {"guid", model::added_guid(a.registry, a.id)}});
        json key = {{"exeSha256", d.env.exe_sha256}, {"side", d.env.side}, {"kiln", d.env.kiln_version}, {"decoder", model::kDecoderVersion},
                    {"regmapRecord", d.meta.regmap.record}, {"order", json::array()}};
        for (const auto& o : d.meta.order) key["order"].push_back({{"mod", o.mod}, {"version", o.version}, {"inputs", o.edits_hash}});
        const json plan = {{"format", 1}, {"key", key}, {"boot", bootj}, {"live", livej}, {"adds", adds}};
        const std::string hash = sha_hex(plan.dump());
        if (d.env.write_data && d.env.write_data("plans/" + hash + ".json", plan.dump(1))) d.meta.plan = {hash, "kiln/data/content/registry/plans/" + hash + ".json", ""};
        const json doc = ::kc::registry::report_json(d.schema, d.result, d.meta);
        d.report = doc.dump();
        if (d.env.write_report && !d.env.write_report(doc.dump(1))) d.log(1, "registry: state/registry.json could not be written");
        // refusals first (warnings, never crowded out by conflict lines), then the conflict lines, then one total
        const std::vector<std::string> all = report_log(d.result);  // refusals first (warnings), then conflicts, then the total
        for (size_t i = 0; i < all.size(); ++i) {
            const bool refusal = all[i].rfind("registry: refused ", 0) == 0, total = i + 1 == all.size();
            d.log(refusal || (total && !d.result.refused.empty()) ? 1 : 0, all[i]);
        }
        d.log(0, "registry: the stage ran (" + d.meta.order_source + " order, " + std::to_string(d.meta.order.size()) + " mod(s)): " + std::to_string(d.counts.edits) +
                     " edit(s) in effect or waiting for the world, " + std::to_string(d.counts.refused) + " refused, " + std::to_string(d.counts.conflicts) +
                     " conflict line(s); restart fields are final (phase start)");
        phase_.store(Phase::Start);
        gen = d.generation;
        for (const auto& [h, s] : d.ready_subs) ready.push_back(s.fn);
        live = {&d.schema, &d.rm, &d.result, &d.ids, &d.nums, &d.base, &d.old, &d.inputs, &d.decoded, nullptr};
        live.report = [this](const Result& r) {
            std::lock_guard l2(d_->mu);
            d_->result = r;
            ++d_->generation;
            d_->meta.generation = d_->generation;
            d_->meta.world_load = true;  // the live back-end applied the now and worldLoad fields (a world's load, a pack reload)
            phase_.store(Phase::WorldLoad);
            d_->meta.written = d_->env.now ? d_->env.now() : "";
            const json doc2 = ::kc::registry::report_json(d_->schema, d_->result, d_->meta);
            d_->report = doc2.dump();
            if (d_->env.write_report) d_->env.write_report(doc2.dump(1));
        };
    }
    if (live_) live_(live);
    else {
        bool any = false;
        for (const auto& [ref, fo] : d.result.fields) {
            const RegistrySpec* rs = d.schema.registry(ref.registry);
            const FieldSpec* f = rs && !is_raw_field(ref.field) ? rs->field(ref.field) : nullptr;
            any = any || !f || f->applies != Applies::Restart;
        }
        if (any) d.log(1, "registry: this build of kiln.content has no live back-end: now and worldLoad fields are composed and reported, not applied");
    }
    for (const auto& fn : ready) fn(Phase::Start, gen);
}

uint64_t Runner::generation() const {
    std::lock_guard lk(d_->mu);
    return d_->generation;
}
Runner::Counts Runner::counts() const {
    std::lock_guard lk(d_->mu);
    return d_->counts;
}

std::string Runner::schema_json() {
    std::lock_guard lk(d_->mu);
    d_->load_regmap();
    json regs = json::object();
    for (const auto& [name, reg] : d_->schema.registries) {
        json fields = json::object();
        for (const auto& [fn, f] : reg.fields) {
            std::string reason;
            const map::Mapping mp = d_->rm.mapping(name, fn, &reason);
            json row = {{"applies", applies_word(f.applies)}, {"readOnly", f.read_only},
                        {"mapping", mp == map::Mapping::Mapped ? "mapped" : mp == map::Mapping::Unmapped ? "unmapped" : mp == map::Mapping::Unproven ? "unproven" : "removed"}};
            if (!reason.empty()) row["reason"] = reason;
            if (f.min) row["min"] = *f.min;
            if (f.max) row["max"] = *f.max;
            if (!f.enum_values.empty()) row["values"] = f.enum_values;
            if (!f.ref.empty()) row["ref"] = f.ref;
            fields[fn] = std::move(row);
        }
        regs[name] = {{"version", reg.version}, {"experimental", "KILNX031"}, {"fields", fields}};
    }
    return json{{"registrySchema", d_->schema.format}, {"registries", regs}}.dump();
}

bool Runner::field_info(const std::string& q, FieldInfo* out) {
    const size_t dot = q.find('.');
    if (dot == std::string::npos) return false;
    const std::string reg = q.substr(0, dot), field = q.substr(dot + 1);
    std::lock_guard lk(d_->mu);
    d_->load_regmap();
    const RegistrySpec* rs = d_->schema.registry(reg);
    const FieldSpec* f = rs ? rs->field(field) : nullptr;
    if (!f) return false;
    out->applies = static_cast<uint32_t>(f->applies);
    out->mapping = static_cast<uint32_t>(d_->rm.mapping(reg, generic_path(field), &out->reason));
    out->type = f->type == FieldType::Int ? 1 : f->type == FieldType::Float ? 2 : f->type == FieldType::Bool ? 3 : f->type == FieldType::Enum ? 4 : f->type == FieldType::Id ? 5 : 6;
    out->read_only = f->read_only;
    if (out->mapping == 0) out->reason.clear();
    return true;
}

KilnResult Runner::read(uint32_t view, const std::string& registry, const std::string& id, const std::string& field, std::string* out) {
    std::lock_guard lk(d_->mu);
    const RegistrySpec* rs = d_->schema.registry(registry);
    if (!rs || view > 2 || (!rs->field(field) && !is_raw_field(field))) return KILN_ERR_INVALID_ARGUMENT;
    const FieldRef ref{registry, lc(id), field};
    if (view == 1) {
        if (!(d_->stage_thread_set && d_->stage_thread == std::this_thread::get_id())) return KILN_ERR_WRONG_THREAD;
        if (!d_->turn || !d_->turn->view) return KILN_ERR_UNAVAILABLE;
        if (!d_->turn->view->exists(registry, ref.id)) return KILN_ERR_NOT_FOUND;
        const auto v = d_->turn->view->get(ref);
        *out = v ? v->to_json().dump() : "null";
        return KILN_OK;
    }
    const Phase ph = phase_.load();
    if (view == 2 && ph != Phase::Start && ph != Phase::WorldLoad) return KILN_ERR_UNAVAILABLE;
    if (view == 0 && ph == Phase::None) return KILN_ERR_UNAVAILABLE;
    if (view == 2) {
        for (const EntryRef& e : d_->result.removed)
            if (e.registry == registry && e.id == ref.id) return KILN_ERR_NOT_FOUND;
        if (const auto it = d_->result.fields.find(ref); it != d_->result.fields.end()) return *out = it->second.result.to_json().dump(), KILN_OK;
    }
    const auto reg = d_->base.registries.find(registry);
    if (reg != d_->base.registries.end())
        for (const BaseModel::Entry& e : reg->second) {
            if (e.id != ref.id || !e.removed_in.empty()) continue;
            const auto f = e.fields.find(field);
            if (f == e.fields.end()) return KILN_ERR_UNAVAILABLE;  // not read in this start
            return *out = f->second.to_json().dump(), KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

KilnResult Runner::select(uint32_t view, const std::string& registry, const std::string& selector, std::string* out) {
    const json s = json::parse(selector, nullptr, false);
    std::string glob;
    std::vector<std::string> has, has_not;
    if (s.is_string()) glob = s.get<std::string>();
    else if (s.is_object() && s.contains("id") && s["id"].is_string()) {
        glob = s["id"].get<std::string>();
        if (s.contains("where")) return KILN_ERR_UNAVAILABLE;  // P2
        if (s.contains("has")) has = s["has"].get<std::vector<std::string>>();
        if (s.contains("hasNot")) has_not = s["hasNot"].get<std::vector<std::string>>();
    } else {
        return KILN_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lk(d_->mu);
    if (!d_->schema.registry(registry) || view > 2) return KILN_ERR_INVALID_ARGUMENT;
    json ids = json::array();
    auto take = [&](const std::string& id, const std::set<std::string>& traits) {
        if (!glob_ci(glob, id)) return;
        for (const std::string& t : has)
            if (!traits.count(t)) return;
        for (const std::string& t : has_not)
            if (traits.count(t)) return;
        ids.push_back(id);
    };
    if (view == 1) {
        if (!(d_->stage_thread_set && d_->stage_thread == std::this_thread::get_id())) return KILN_ERR_WRONG_THREAD;
        if (!d_->turn || !d_->turn->view) return KILN_ERR_UNAVAILABLE;
        for (const std::string& id : d_->turn->view->ids(registry)) {
            std::set<std::string> traits;
            for (const std::string& t : has) if (d_->turn->view->has_trait(registry, id, t)) traits.insert(t);
            for (const std::string& t : has_not) if (d_->turn->view->has_trait(registry, id, t)) traits.insert(t);
            take(id, traits);
        }
        return *out = ids.dump(), KILN_OK;
    }
    const Phase ph = phase_.load();
    if (ph == Phase::None || (view == 2 && ph != Phase::Start && ph != Phase::WorldLoad)) return KILN_ERR_UNAVAILABLE;
    std::set<std::string> removed;
    if (view == 2)
        for (const EntryRef& e : d_->result.removed)
            if (e.registry == registry) removed.insert(e.id);
    if (const auto reg = d_->base.registries.find(registry); reg != d_->base.registries.end())
        for (const BaseModel::Entry& e : reg->second)
            if (e.removed_in.empty() && !removed.count(e.id)) take(e.id, e.traits);
    if (view == 2)
        for (const EntryRef& a : d_->result.added)
            if (a.registry == registry) take(a.id, {});
    return *out = ids.dump(), KILN_OK;
}

KilnResult Runner::report_json(std::string* out) {
    std::lock_guard lk(d_->mu);
    const Phase ph = phase_.load();
    if (ph != Phase::Start && ph != Phase::WorldLoad) return KILN_ERR_UNAVAILABLE;
    *out = d_->report;
    return KILN_OK;
}

}  // namespace kc::registry::stage
