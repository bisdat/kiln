// SPDX-License-Identifier: MIT
// kiln.content registry stage: the stage runner (stage.h).
#include "stage.h"
#include "saves.h"
#include "unmapped.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "services/kiln_caps.h"
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

static_assert(std::size(gen::kRegistries) + std::size(gen::kFields) < KILN_CAP_OWNER_MAX);

// In increasing order of registry coverage. A registry is available if any of its fields is verified here.
enum class CapCoverage { WrongSide, Unmapped, Unverified, Verified };

const char* only_side(gen::Side side) {
    if (side == gen::Side::Client) return "client";
    if (side == gen::Side::Server) return "server";
    return nullptr;
}

std::string field_cap_name(const std::string& prefix, const char* path) {
    std::string name = prefix + "." + path;
    for (size_t pos = name.find("[]"); pos != std::string::npos; pos = name.find("[]", pos)) name.erase(pos, 2);
    return name;
}

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
            if (!e.drop_groups.empty()) row["dropGroups"] = e.drop_groups;
            a.push_back(std::move(row));
        }
        regs[reg] = std::move(a);
    }
    json cover = json::object();
    for (const auto& [reg, m] : b.raw_cover)
        for (const auto& [k, c] : m) cover[reg][k] = {{"field", c.field}, {"converted", c.converted}};
    return {{"entryMetadata", 1}, {"registries", regs}, {"rawCover", cover}, {"retired", b.retired}};
}
bool base_in(const json& j, BaseModel* b) {
    if (!j.is_object() || j.value("entryMetadata", 0) != 1 || !j.contains("registries") || !j["registries"].is_object()) return false;
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
            if (row.contains("dropGroups")) e.drop_groups = row["dropGroups"].get<std::vector<std::pair<size_t, size_t>>>();
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

// ---- plans (registry-rollback) ----
bool is_hash(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
// A plan's name: the SHA-256 of its document without "previous" (the link to the last good plan, which a later start may
// rewrite; the content it names never changes).
std::string plan_hash_of(json plan) {
    plan.erase("previous");
    return sha_hex(plan.dump());
}
bool has_registry_row(const FieldOutcome& fo) {
    return std::any_of(fo.chain.begin(), fo.chain.end(), [](const ChainRow& c) { return c.kind == WriterKind::Registry; });
}
// "items[game:plank].maxStack" (FieldRef::text) back into its parts. false: not that shape.
bool field_ref_of(const std::string& text, FieldRef* out) {
    const size_t open = text.find('['), close = text.find("].");
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    *out = {text.substr(0, open), text.substr(open + 1, close - open - 1), text.substr(close + 2)};
    return !out->registry.empty() && !out->id.empty() && !out->field.empty();
}
std::string short_hash(const std::string& h) { return h.substr(0, 12); }

}  // namespace

// ---- the base cache's key (review finding 5) ----
std::string keys_digest(const std::vector<KilnAssetKey>& keys) {
    std::string s;
    for (const KilnAssetKey& k : keys) s += key_text(k) + "\n";
    return sha_hex(s);
}
// Every type the model lists, with a digest of its keys: a resource another archive writer adds (or removes) misses the cache.
class Inventory final : public model::Source {
   public:
    explicit Inventory(model::Source& in) : in_(in) {}
    const char* name() const override { return in_.name(); }
    bool list(const std::string& type, std::vector<KilnAssetKey>* out, std::string* why) override {
        const bool ok = in_.list(type, out, why);
        if (ok) lists_[type] = keys_digest(*out);
        return ok;
    }
    bool count(const model::Place& p, size_t* n, std::string* why) override { return in_.count(p, n, why); }
    bool member(const model::Place& p, const std::string& path, json* out, std::string* why) override { return in_.member(p, path, out, why); }
    std::string digest() override { return in_.digest(); }
    std::vector<std::pair<KilnAssetKey, std::string>> keys_read() const override { return in_.keys_read(); }
    bool touch(const KilnAssetKey& key, const std::string& type, std::string* why) override { return in_.touch(key, type, why); }
    void release() override { in_.release(); }
    const std::map<std::string, std::string>& lists() const { return lists_; }

   private:
    model::Source& in_;
    std::map<std::string, std::string> lists_;
};
// The loaded regmap's content (common rows and the build's record): a regenerated record under the same version string misses.
std::string regmap_digest(const map::RegMap& rm) {
    json j = json::object();
    for (const auto& [n, r] : rm.registries) {
        const map::EntrySource& e = r.entries;
        json rows = json::object();
        for (const auto& [f, row] : r.rows)
            rows[f] = {row.paths, row.conv, row.values, row.formula, row.config, static_cast<int>(row.read), static_cast<int>(row.backend),
                       static_cast<int>(row.state), row.client_needs};
        j["registries"][n] = {{"entries", {static_cast<int>(e.kind), e.resource_type, e.array, e.resource_guid, e.name, e.guid, e.id, e.unnamed, e.name_from, e.key}},
                              {"rows", rows}};
    }
    if (rm.has_build) {
        json rows = json::object();
        for (const auto& [k, v] : rm.build.rows) rows[k] = static_cast<int>(v);
        j["build"] = {{"side", rm.build.side}, {"exe", rm.build.exe_sha256}, {"gameBuild", rm.build.game_build}, {"verified", rm.build.verified},
                      {"regmapDigest", rm.build.regmap_digest}, {"commonDigest", rm.common_digest},
                      {"rows", rows}, {"layouts", rm.build.layouts}, {"pinned", rm.build.aliases.pinned}, {"removed", rm.build.aliases.removed}};
    }
    return sha_hex(j.dump());
}

// Stable full models share a family across mod sets; raw requests are never serialized into this cache.
std::string cache_family(const Env& env, const map::RegMap& rm, const model::Need& need) {
    const json request = need.all && need.raw.empty() ? json{{"all", true}} : need_json(need);
    return sha_hex(std::string(model::kDecoderVersion) + "\nentry-metadata-1\n" + env.kiln_version + "\n" + env.side + "\n" + env.exe_sha256 + "\n" +
        (rm.has_build ? (rm.build.verified ? "verified" : "unverified") : "none") + "\n" + regmap_digest(rm) + "\n" + request.dump());
}
std::string cache_path(const std::string& family) { return "cache/" + family.substr(0, 32) + ".json"; }
bool read_base_cache(const Env& env, const std::string& family, model::Source& source, LiveData* out) {
    if (!env.read_data) return false;
    const auto text = env.read_data(cache_path(family));
    if (!text) return false;
    try {
        const json c = json::parse(*text, nullptr, false);
        if (!c.is_object() || c.value("family", std::string()) != family || !c.at("keys").is_array() || !c.at("lists").is_object()) return false;
        for (auto it = c["lists"].begin(); it != c["lists"].end(); ++it) {
            std::vector<KilnAssetKey> now; std::string why;
            if (!it.value().is_string() || !source.list(it.key(), &now, &why) || keys_digest(now) != it.value().get<std::string>()) return false;
        }
        for (const auto& k : c["keys"]) {
            KilnAssetKey key{}; std::string why;
            if (!k.is_array() || k.size() != 2 || !key_parse(k[0].get<std::string>(), &key) || !source.touch(key, k[1].get<std::string>(), &why)) return false;
        }
        LiveData data;
        if (source.digest() != c.value("digest", std::string()) || !base_in(c.at("base"), &data.base)) return false;
        data.ids = c.at("ids").get<std::map<std::string, std::string>>();
        data.nums = c.at("nums").get<std::map<std::string, uint32_t>>();
        data.decoded = c.at("decoded").get<std::set<std::string>>();
        *out = std::move(data);
        if (env.log) env.log(0, "registry: the base of this start is the one the cache holds (" + std::to_string(c["keys"].size()) +
            " resource(s), digest " + c.value("digest", std::string()).substr(0, 16) + "): not decoded again");
        return true;
    } catch (const std::exception&) { return false; } // damaged cache is a miss, never a partial model
}
void write_base_cache(const Env& env, const std::string& family, model::Source& source, const Inventory& inventory,
                      const BaseModel& base, const std::map<std::string, std::string>& ids,
                      const std::map<std::string, uint32_t>& nums, const std::set<std::string>& decoded) {
    if (!env.write_data) return;
    const std::string digest = source.digest();
    if (digest.empty()) return;
    json keys = json::array();
    for (const auto& [key, type] : source.keys_read()) keys.push_back({key_text(key), type});
    env.write_data(cache_path(family), json{{"format", 1}, {"family", family}, {"digest", digest}, {"keys", keys},
        {"lists", inventory.lists()}, {"base", base_out(base)}, {"ids", ids}, {"nums", nums}, {"decoded", decoded}}.dump());
}

// ---- inputs ----
void refuse_unmapped(const map::RegMap& rm, const Schema& schema, Batch* b, std::vector<Refusal>* refused,
                     const std::function<bool(const std::string&, bool, std::string*)>& support = {}) {
    refuse_unmapped_edits(rm, schema, b, refused, bool(support));
    if (!support) return;
    std::erase_if(b->edits, [&](const Edit& edit) {
        if (edit.entry == EntryOp::None) return false;
        std::string reason;
        if (support(edit.registry, edit.entry == EntryOp::Add, &reason)) return false;
        Refusal row{b->mod, b->source, b->stage, edit.n, reason, "", edit.registry, edit.add_id, ""};
        if (edit.entry == EntryOp::Add) row.entry = edit.registry + "|" + edit.add_id;
        else if (edit.select.exact()) row.id = edit.select.id;
        refused->push_back(std::move(row)); return true;
    });
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
    bool registry_users = false;  // set_order named a mod whose code uses the registry API (review4 NS1)
    bool order_set = false;
    struct Sub {
        uint64_t owner = 0;
        std::string mod;
        StageFn fn;
    };
    std::map<uint64_t, Sub> stage_subs;
    struct Ready {
        uint64_t owner = 0;
        ReadyFn fn;
    };
    std::map<uint64_t, Ready> ready_subs;
    uint64_t next_handle = 1;
    struct Queued {
        uint64_t owner = 0;
        Batch batch;
        std::vector<Refusal> refused;
    };
    std::map<std::string, std::vector<Batch>> recorded; // successful callback turns kept for live recomposition
    std::map<std::string, std::vector<Queued>> queued;  // mod -> its submits, retaining each caller's owner
    std::set<uint64_t> gone;  // owner ids are never reused
    bool alive(uint64_t owner) const { return !gone.count(owner) && (!env.owner_alive || env.owner_alive(owner)); }
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
    Result result, live_result;
    bool full_live = false, installing_live = false;
    std::unique_ptr<model::Source> lazy_source;
    OldWritesFn lazy_old;
    std::string lazy_error;
    std::vector<OldWrite> old;
    std::vector<ModInput> inputs;
    std::vector<Refusal> pre_refused;
    std::vector<Refusal> outside;                    // every refusal made outside compose (the live back-end keeps them on a reload)
    std::map<FieldRef, FieldOutcome> boot_fields;    // restart fields as the boot back-end wrote them
    std::map<std::string, std::string> ids;
    std::map<std::string, uint32_t> nums;
    std::set<std::string> decoded;
    uint64_t generation = 0;
    Counts counts;
    std::string report;
    ReportMeta meta;
    std::vector<BootResource> boot;
    std::map<std::string, uint64_t> edits_by_mod;
    // plans (registry-rollback): plans/index.json as this start read it (the last good plan, the plan the last world ran on),
    // and the plan this start runs on when a rollback applied (its hash and its own "previous")
    json index = json::object();
    std::string rollback, rollback_previous;
    // registry.json "saves" (saves.h): this start's notes from the stage itself (the max stack against the plan the last world
    // ran on), and the lines already in kiln.log (each once per start)
    std::vector<saves::SaveNote> stage_notes;
    std::set<std::string> notes_logged;
    json saves_json(const Result& r) {
        std::vector<saves::SaveNote> all = stage_notes;
        for (saves::SaveNote& n : saves::notes(r)) all.push_back(std::move(n));
        std::vector<std::string> lines;
        for (const saves::SaveNote& n : all) {
            const std::string line = "registry: saves: " + n.field.text() + " " + n.text + " [" + n.decision + "]";
            if (notes_logged.insert(line).second) log(n.decision == "report" ? 0 : 1, line);
        }
        return all.empty() ? json(nullptr) : saves::notes_json(all);
    }

    void log(int level, const std::string& line) {
        if (env.log) env.log(level, line);
    }
    void revoked_queue(const std::string& mod, uint64_t owner, size_t batches, size_t edits, Stage stage) {
        if (!batches) return;
        const std::string why = "owner " + std::to_string(owner) + " was revoked; dropped " + std::to_string(batches) +
            " queued batch(es), " + std::to_string(edits) + " edit(s) before the stage";
        log(1, "registry: mod " + mod + " " + why);
        pre_refused.push_back({mod, "owner:" + std::to_string(owner), stage, 0, why, ""});
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

    // kiln/data/content/registry/rollback.json asks this start to run on an earlier plan's edits. true: *out holds the plan's
    // inputs, read and mapping-checked on this build, every mod of it loaded at the plan's version. false: *why says why the
    // plan cannot start (nothing of it is used). *rb: registry.json's "rollback" so far; *from: the values of the plan rolled
    // away from (the "changedBack" rows compare against them).
    bool rollback_inputs(const std::string& request, const std::set<std::string>& loaded, const std::map<std::string, std::string>& versions,
                         std::vector<ModInput>* out, json* rb, json* from, std::string* why) {
        const json q = json::parse(request, nullptr, false);
        if (!q.is_object() || q.value("format", 0) != 1 || !q.contains("plan") || !q["plan"].is_string() || !is_hash(q["plan"].get<std::string>())) {
            *rb = {{"plan", nullptr}};
            return *why = "the rollback request (kiln/data/content/registry/rollback.json) cannot be read: it is {\"format\": 1, \"plan\": \"<plan hash>\"}", false;
        }
        const std::string plan = q["plan"].get<std::string>();
        const std::string from_hash = q.contains("from") && q["from"].is_string() && is_hash(q["from"].get<std::string>()) ? q["from"].get<std::string>() : "";
        *rb = {{"plan", plan}, {"from", from_hash.empty() ? json(nullptr) : json(from_hash)}, {"requested", q.value("requested", "")}};
        const std::string name = "plan " + short_hash(plan);
        const auto text = env.read_data("plans/" + plan + ".json");
        if (!text) return *why = name + " is not in kiln/data/content/registry/plans (it was removed or never written)", false;
        const json p = json::parse(*text, nullptr, false);
        if (!p.is_object()) return *why = name + " cannot be read (not JSON)", false;
        if (p.value("format", 0) != 2 || !p.contains("inputs") || !p["inputs"].is_array())
            return *why = name + " was written by an older Kiln and holds no edits to start on", false;
        if (plan_hash_of(p) != plan) return *why = name + "'s file does not match its name (it was changed after Kiln wrote it)", false;
        const json key = p.value("key", json::object());
        if (key.value("side", env.side) != env.side) return *why = name + " is the " + key.value("side", std::string()) + "'s; this is the " + env.side, false;
        if (key.value("exeSha256", env.exe_sha256) != env.exe_sha256) (*rb)["madeOnExe"] = key.value("exeSha256", std::string());
        std::vector<std::string> differ;
        for (const json& m : p["inputs"]) {
            const std::string mod = m.value("mod", std::string()), version = m.value("version", std::string());
            if (!loaded.count(mod)) { differ.push_back(mod + " is gone (not loaded in this start)"); continue; }
            const auto v = versions.find(mod);
            const std::string now = v == versions.end() ? std::string() : v->second;
            if (now != version) differ.push_back(mod + " changed (" + version + ", now " + now + ")");
        }
        if (!differ.empty()) {
            std::string all;
            for (const std::string& x : differ) all += (all.empty() ? "" : "; ") + x;
            return *why = name + "'s mods are not this start's: " + all, false;
        }
        for (const json& m : p["inputs"]) {
            ModInput in;
            in.id = m.value("mod", std::string());
            for (const json& doc : m.value("batches", json::array())) {
                Batch b;
                std::vector<Refusal> refused;
                std::string err;
                if (!parse_batch(doc, schema, &b, &refused, &err)) return *why = name + ": an edit document of " + in.id + " cannot be read on this Kiln: " + err, false;
                if (lc(b.mod) != lc(in.id)) return *why = name + ": a batch of " + in.id + " names mod " + b.mod, false;
                refuse_unmapped(rm, schema, &b, &refused, env.entry_support);
                if (!refused.empty()) {
                    const Refusal& x = refused.front();
                    return *why = name + ": " + x.mod + " " + x.source + " #" + std::to_string(x.n) + " is refused on this build: " + x.reason, false;
                }
                in.batches.push_back(std::move(b));
            }
            out->push_back(std::move(in));
        }
        (*rb)["previous"] = p.contains("previous") && p["previous"].is_string() ? p["previous"] : json(nullptr);
        *from = json::object();
        if (!from_hash.empty())
            if (const auto ft = env.read_data("plans/" + from_hash + ".json")) {
                const json f = json::parse(*ft, nullptr, false);
                if (f.is_object() && f.contains("values") && f["values"].is_object()) *from = f["values"];
            }
        return true;
    }

    void report_caps() {
        if (!env.report_cap) return;
        const bool verified_build = rm.verified();  // a record of another regmap/common enables nothing (review3 S4)
        auto publish = [&](const std::string& name, CapCoverage coverage, const std::string& detail) {
            const char* status = "unmapped on this game build";
            uint32_t reason = KILN_CAP_REASON_SYMBOL_MISSING;
            switch (coverage) {
                case CapCoverage::WrongSide:
                    status = "wrong side";
                    reason = KILN_CAP_REASON_WRONG_SIDE;
                    break;
                case CapCoverage::Unmapped:
                    break;
                case CapCoverage::Unverified:
                    status = verified_build ? "unverified mapping on this game build" : "unverified regmap record for this game build";
                    if (!verified_build) reason = KILN_CAP_REASON_POLICY;
                    break;
                case CapCoverage::Verified:
                    status = "mapped and verified";
                    reason = KILN_CAP_REASON_NONE;
                    break;
            }
            const uint32_t state = reason == KILN_CAP_REASON_NONE ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE;
            const KilnResult rc = env.report_cap(name, state, reason, std::string(status) + "; " + detail);
            if (rc != KILN_OK) log(1, "registry: capability " + name + " could not be reported (rc " + std::to_string(rc) + ")");
        };
        for (const auto& reg : gen::kRegistries) {
            const std::string prefix = std::string("registry.") + reg.name;
            CapCoverage best = CapCoverage::WrongSide;
            size_t verified_fields = 0;
            for (size_t i = reg.first; i < reg.first + reg.count; ++i) {
                const auto& f = gen::kFields[i];
                std::string detail = std::string("applies: ") + applies_word(schema_detail::applies_of(f.applies));
                CapCoverage coverage = CapCoverage::WrongSide;
                const char* side = only_side(f.side);
                if (side && env.side != side) {
                    detail += std::string("; ") + side + " only";
                } else {
                    std::string edit_reason;
                    const auto mapping = rm.mapping(reg.name, f.path, &edit_reason);
                    coverage = mapping == map::Mapping::Mapped ? CapCoverage::Verified :
                               mapping == map::Mapping::Unproven ? CapCoverage::Unverified : CapCoverage::Unmapped;
                }
                if (coverage == CapCoverage::Verified) ++verified_fields;
                best = std::max(best, coverage);
                publish(field_cap_name(prefix, f.path), coverage, detail);
            }
            publish(prefix, best, std::to_string(verified_fields) + " of " + std::to_string(reg.count) + " fields verified on this side");
        }
        if (env.entry_support) for (const auto& [name, spec] : schema.registries) {
            for (const bool add : {true, false}) {
                if (add ? !spec.add : !spec.remove) continue;
                std::string reason;
                bool available = env.entry_support(name, add, &reason);
                if (available && !add && name == "items") {
                    for (const char* field : {"entries[].chance", "entries[].weight"})
                        if (rm.mapping("drops", field, &reason) != map::Mapping::Mapped) { available = false; reason = "drop tables not proven on this build"; break; }
                    if (available) for (const char* field : {"inputs", "outputs", "inputs[].item", "outputs[].item"})
                        if (rm.mapping("recipes", field, &reason) != map::Mapping::Mapped) { available = false; reason = "recipe dependencies not proven on this build"; break; }
                }
                const std::string capability = "registry." + name + (add ? ".add" : ".remove");
                env.report_cap(capability, available ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE,
                    available ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_POLICY,
                    "GameStart only; " + (available ? std::string("entry backend ready") : reason));
            }
        }
    }
};

Runner::Runner(Env env) : d_(std::make_unique<Impl>()) { d_->env = std::move(env); }
Runner::~Runner() = default;

KilnResult Runner::set_order(const std::string& order_json) {
    const json j = json::parse(order_json, nullptr, false);
    if (!j.is_array()) return KILN_ERR_INVALID_ARGUMENT;
    std::vector<std::pair<std::string, std::string>> order;
    std::set<std::string> seen;
    bool users = false;
    for (const auto& row : j) {
        if (!row.is_object() || !row.contains("mod") || !row["mod"].is_string()) return KILN_ERR_INVALID_ARGUMENT;
        const std::string mod = row["mod"].get<std::string>();
        if (!seen.insert(mod).second) return KILN_ERR_INVALID_ARGUMENT;
        order.emplace_back(mod, row.contains("version") && row["version"].is_string() ? row["version"].get<std::string>() : std::string());
        users |= row.contains("registry") && row["registry"].is_boolean() && row["registry"].get<bool>();
    }
    std::lock_guard lk(d_->mu);
    if (phase_.load() != Phase::None) return KILN_ERR_UNAVAILABLE;
    d_->order = std::move(order);
    d_->order_set = true;
    d_->registry_users = users;
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
    if (!d_->alive(owner)) return KILN_ERR_REVOKED;
    if (phase_.load() != Phase::None) return KILN_ERR_UNAVAILABLE;
    for (const auto& [h, s] : d_->stage_subs)
        if (s.mod == mod) return KILN_ERR_INVALID_ARGUMENT;
    *handle = d_->next_handle++;
    d_->stage_subs[*handle] = {owner, mod, std::move(fn)};
    return KILN_OK;
}

KilnResult Runner::subscribe_ready(uint64_t owner, ReadyFn fn, uint64_t* handle) {
    if (!owner || !fn || !handle) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lk(d_->mu);
    if (!d_->alive(owner)) return KILN_ERR_REVOKED;
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
    if (!owner) return;
    {
        std::lock_guard lk(d_->mu);
        d_->gone.insert(owner);
        for (auto it = d_->stage_subs.begin(); it != d_->stage_subs.end();)
            it = it->second.owner == owner ? d_->stage_subs.erase(it) : std::next(it);
        for (auto it = d_->ready_subs.begin(); it != d_->ready_subs.end();)
            it = it->second.owner == owner ? d_->ready_subs.erase(it) : std::next(it);
        for (auto it = d_->queued.begin(); it != d_->queued.end();) {
            size_t batches = 0, edits = 0;
            Stage stage = Stage::Alter;
            std::erase_if(it->second, [&](const Impl::Queued& q) {
                if (q.owner != owner) return false;
                if (!batches) stage = q.batch.stage;
                ++batches; edits += q.batch.edits.size() + q.refused.size();
                return true;
            });
            d_->revoked_queue(it->first, owner, batches, edits, stage);
            it = it->second.empty() ? d_->queued.erase(it) : std::next(it);
        }
    }
    if (d_->env.owner_gone) d_->env.owner_gone(owner); // never call out while holding the runner lock
}

void Runner::notify_ready(const std::vector<uint64_t>& handles, Phase phase, uint64_t generation) {
    for (const uint64_t h : handles) {
        ReadyFn fn;
        {
            std::lock_guard lk(d_->mu);
            const auto it = d_->ready_subs.find(h);
            if (it == d_->ready_subs.end()) continue;  // removed by an earlier callback in this same snapshot
            if (!d_->alive(it->second.owner)) {
                d_->ready_subs.erase(it);
                continue;
            }
            fn = it->second.fn;  // dispatch starts here; revoke/unsubscribe do not wait for an in-flight callback
        }
        if (fn(phase, generation)) continue;
        {
            std::lock_guard lk(d_->mu);
            d_->ready_subs.erase(h);
        }
        d_->log(2, "registry: a ready callback faulted and was removed");
    }
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
        if (!d_->alive(owner)) {
            err = "the caller's owner is revoked: the batch is refused";
            return say(KILN_ERR_REVOKED);
        }
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
    if (!d_->alive(owner)) {
        err = "the caller's owner is revoked: the batch is refused";
        b.edits.clear();
        return say(KILN_ERR_REVOKED);
    }
    const Phase ph = phase_.load();
    if (ph == Phase::None) {
        if (b.source.empty()) b.source = "code:" + b.mod;
        d_->queued[b.mod].push_back({owner, b, refused});
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
    refuse_unmapped(d_->rm, d_->schema, &b, &refused, d_->env.entry_support);
    Batch* out = d_->turn->out;
    if (out->source.empty()) out->source = "code:" + b.mod;
    for (Edit& e : b.edits) {
        e.n = static_cast<uint32_t>(out->edits.size());
        out->edits.push_back(e);
    }
    d_->pre_refused.insert(d_->pre_refused.end(), refused.begin(), refused.end());
    return say(KILN_OK);
}

std::vector<BootResource> Runner::run(const OldWritesFn& old_fn) { return run(old_fn, {}); }

std::vector<BootResource> Runner::run(const OldWritesFn& old_fn, const std::vector<std::pair<KilnAssetKey, std::string>>& freeze) {
    Impl& d = *d_;
    std::vector<ModInput> inputs;
    {
        std::lock_guard lk(d.mu);
        if (phase_.load() != Phase::None) return {};
        d.stage_thread = std::this_thread::get_id();
        d.stage_thread_set = true;
        phase_.store(Phase::Add);
        d.load_regmap();
    }
    // Reports can invoke capability subscribers synchronously, so keep the stage mutex out of the call.
    d.report_caps();
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
    std::set<std::string> loaded;
    for (const auto& [m, v] : order) versions[m] = v, loaded.insert(m);
    std::vector<Refusal> skipped;
    for (Env::DataFile& f : files) {
        // review finding 3 (ruling 2026-11-21): only mods Kiln.Host loaded on this side, in its order; the others are skipped
        // and named. With Kiln.Host's order, membership decides alone: on a client it holds side "server" mods when hosted
        // server mods are on, since a hosted world's server runs in this process (recheck N1). Without .NET (the folders'
        // order), a mod for the other side is skipped.
        std::string why;
        if (d.meta.order_source == "kiln.host") {
            if (!loaded.count(f.mod)) why = "Kiln.Host did not load the mod on this side (not in its load order)";
        } else if (!(f.side.empty() || f.side == "both" || f.side == d.env.side)) {
            why = "the mod is for the " + f.side + " side, this is the " + d.env.side;
        }
        if (!why.empty()) {
            skipped.push_back({f.mod, f.rel, Stage::Alter, 0, f.rel + ": " + why + "; its registry section was not applied", ""});  // kiln.log: report_log
            continue;
        }
        ModInput& in = by_mod[f.mod];
        in.id = f.mod;
        if (versions[f.mod].empty()) versions[f.mod] = f.version;
        if (!f.error.empty()) {
            in.refused.push_back({f.mod, f.rel, Stage::Alter, 0, f.rel + ": " + f.error + "; nothing of this file's registry section was applied", ""});  // kiln.log: report_log
        }
        for (Batch& b : f.batches) in.batches.push_back(std::move(b));
        in.refused.insert(in.refused.end(), f.refused.begin(), f.refused.end());
    }
    {
        std::lock_guard lk(d.mu);
        // The stage takes the surviving queue here. Already-taken stage edits stay until the next start on revoke.
        for (auto& [mod, qs] : d.queued) {
            for (Impl::Queued& q : qs) {
                if (!d.alive(q.owner)) {
                    d.revoked_queue(mod, q.owner, 1, q.batch.edits.size() + q.refused.size(), q.batch.stage);
                    continue;
                }
                ModInput& in = by_mod[mod];
                in.id = mod;
                in.batches.push_back(std::move(q.batch));
                in.refused.insert(in.refused.end(), q.refused.begin(), q.refused.end());
            }
        }
        d.queued.clear();
        d.pre_refused.insert(d.pre_refused.end(), skipped.begin(), skipped.end());
        for (const auto& [h, s] : d.stage_subs) {
            ModInput& in = by_mod[s.mod];
            in.id = s.mod;
            const std::string mod = s.mod;
            const uint64_t handle = h;
            in.code = [this, mod, handle](Stage st, const View& v, Batch* out) {
                StageFn fn;
                uint64_t owner = 0;
                {
                    std::lock_guard l2(d_->mu);
                    const auto it = d_->stage_subs.find(handle);
                    if (it == d_->stage_subs.end()) return;
                    if (!d_->alive(it->second.owner)) {
                        d_->stage_subs.erase(it);
                        return;
                    }
                    owner = it->second.owner;
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
                if (!d_->alive(owner)) {
                    const size_t dropped = out->edits.size() - before;
                    out->edits.resize(before);
                    // As on fault, keep refusal diagnostics: another live owner may have submitted them.
                    d_->stage_subs.erase(handle);
                    d_->pre_refused.push_back({mod, "code:" + mod, st, 0,
                        "the stage callback's owner was revoked; its " + std::to_string(dropped) + " edit(s) of this turn were dropped", ""});
                } else if (!clean) {
                    const size_t dropped = out->edits.size() - before;
                    out->edits.resize(before);
                    d_->stage_subs.erase(handle);
                    d_->pre_refused.push_back({mod, "code:" + mod, st, 0,
                                               "the stage callback faulted; its " + std::to_string(dropped) + " edit(s) of this stage were dropped and it is not called again", ""});
                    d_->log(2, "registry: mod " + mod + "'s stage callback faulted in " + stage_name(st) + ": its edits of that turn are dropped, the callback is removed");
                } else if (!out->edits.empty()) d_->recorded[mod].push_back(*out);
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
        for (Batch& b : in.batches) refuse_unmapped(d.rm, d.schema, &b, &in.refused, d.env.entry_support);
        inputs.push_back(std::move(in));
    }
    // registry-rollback: the plans' index (the last good plan, the plan the last world ran on), and a rollback request that
    // starts this boot on an earlier plan's edits instead of the mods' current ones (kept until it is cleared)
    json rb;                          // registry.json "rollback"; null: none asked
    json from_values = json::object();
    std::vector<ModInput> fallback;   // the mods' current inputs while the plan's are tried
    bool rolled = false;
    if (d.env.read_data) {
        if (const auto t = d.env.read_data("plans/index.json")) {
            const json idx = json::parse(*t, nullptr, false);
            if (idx.is_object()) d.index = idx;
        }
        if (const auto req = d.env.read_data("rollback.json")) {
            std::vector<ModInput> plan_inputs;
            std::string why;
            if (d.rollback_inputs(*req, loaded, versions, &plan_inputs, &rb, &from_values, &why)) {
                for (const ModInput& in : plan_inputs)
                    for (const Batch& b : in.batches)
                        if (versions[in.id].empty()) versions[in.id] = b.mod_version;
                json left = json::array();
                for (const ModInput& in : inputs)
                    if (std::none_of(plan_inputs.begin(), plan_inputs.end(), [&](const ModInput& x) { return x.id == in.id; })) left.push_back(in.id);
                rb["leftOut"] = std::move(left);
                fallback = std::move(inputs);
                inputs = std::move(plan_inputs);
                rolled = true;
            } else {
                rb["state"] = "refused";
                rb["reason"] = why + "; this start runs on the mods' current registry edits";
            }
        }
    }
    // the base
    std::string why;
    // no mod has registry content: nothing is read (an empty registry.json says so), except once per game build, when the
    // running exe has no id table yet (work-bb 2026-11-21: kilnctl update needs one for every build)
    const std::string ids_rel = "ids-" + d.env.side + "-" + d.env.exe_sha256 + ".json";
    const bool ids_missing = d.env.read_data && d.env.write_data && !d.env.exe_sha256.empty() && !d.env.read_data(ids_rel);
    // the highest max stack a world ever started with, per item (plans/index.json "stacks", never lowered: a save that world
    // wrote can hold such stacks), against this start's value, else the base's: lower, and a save can hold bigger stacks than
    // this start allows (saves.h stackCut)
    const json then = d.index.contains("stacks") && d.index["stacks"].is_object() ? d.index["stacks"] : json::object();
    bool then_stacks = false;
    for (auto it = then.begin(); it != then.end(); ++it) {
        FieldRef ref;
        then_stacks = then_stacks || (field_ref_of(it.key(), &ref) && ref.registry == "items" && ref.field == "maxStack");
    }
    const bool want_base = !inputs.empty() || !fallback.empty() || then_stacks;
    // A mod whose code uses the registry API (Kiln.Host's set_order flag): decode the stable base here, at resources-ready,
    // before kiln.content writes packs into resident resource pages, rather than at first use (review4 NS1).
    bool users = false;
    {
        std::lock_guard lk(d.mu);
        users = d.registry_users;
    }
    const bool eager = want_base || (users && d.env.prepare_live);
    const bool freeze_old = !eager && d.env.prepare_live && !freeze.empty();
    std::unique_ptr<model::Source> src = !eager && !ids_missing && !freeze_old ? nullptr : d.env.source ? d.env.source(&why) : nullptr;
    if ((!eager || (!want_base && !src)) && d.env.prepare_live) d.lazy_old = old_fn; // production supplies owned document copies
    if (freeze_old) {
        for (const auto& [key, type] : freeze) {
            std::string error;
            if (!src || !src->touch(key, type, &error)) {
                d.lazy_error = "the registry's pre-patch base could not be frozen: " + (error.empty() ? why : error);
                d.log(1, "registry: " + d.lazy_error);
                break;
            }
        }
    }
    d.meta.pid = d.env.pid;
    d.meta.kiln = d.env.kiln_version;
    d.meta.regmap = {d.env.side, d.env.exe_sha256, d.rm.has_build ? "regmap/" + d.env.side + "/" + d.env.exe_sha256 + ".json" : "", d.rm.verified()};
    d.meta.base.writer = d.env.writer;
    BaseModel base;
    std::vector<OldWrite> old;
    std::unique_ptr<Inventory> inventory;  // before m: the model reads through it
    std::unique_ptr<model::Model> m;
    std::vector<ModInput> named_inputs = inputs;  // the plan's and the mods' current inputs: either may be composed below
    named_inputs.insert(named_inputs.end(), fallback.begin(), fallback.end());
    model::Need need = model::needs_of(d.schema, named_inputs);
    named_inputs.clear();
    if (then_stacks) need.fields["items"].insert("maxStack"), need.registries.insert("items");
    if (d.env.prepare_live && eager) need.all = true;
    bool hit = false;
    if (!src && !want_base) {
        if (eager) d.log(1, "registry: the base could not be read at the stage (" + why + "); it is prepared at first use instead");
        d.meta.base.origin = "none";
    } else if (!eager) {
        if (ids_missing) {
            // Only the once-per-build id ledger: no stable field decode on unused starts.
            model::Model ids_model(*src, d.rm, d.schema);
            ids_model.list_all();  // entries only (with the reference tables), no field decode
            ids_model.id_table(&d.ids, &d.nums);
            d.env.write_data(ids_rel, ids_model.id_table_json().dump());
            d.log(0, "registry: the id table of this game build (" + d.env.side + " " + d.env.exe_sha256.substr(0, 8) + ") is written to the data folder (" +
                         std::to_string(d.ids.size()) + " entries); no mod has registry content");
            for (const auto& note : ids_model.take_notes()) d.log(0, "registry: " + note);
        }
        if (freeze_old) d.lazy_source = std::move(src);
        else { src->release(); src.reset(); }
        d.meta.base.origin = "none";
    } else if (!src) {
        d.log(2, "registry: the stage has no base to read (" + why + "): nothing of the registry is applied in this start");
        std::lock_guard lk(d.mu);
        d.counts.failed = true;
    } else {
        inventory = std::make_unique<Inventory>(*src);
        m = std::make_unique<model::Model>(*inventory, d.rm, d.schema);
        d.meta.base.origin = src->name();
        const std::string family = cache_family(d.env, d.rm, need);
        if (need.raw.empty() && !ids_missing) {
            LiveData cached;
            hit = read_base_cache(d.env, family, *src, &cached);
            if (hit) {
                base = std::move(cached.base); d.ids = std::move(cached.ids);
                d.nums = std::move(cached.nums); d.decoded = std::move(cached.decoded);
            }
        }
        if (!hit) {
            m->fill(need, &base);
            // the id table of this build for kilnctl update: every R1 registry the first time this build starts with Kiln
            const bool want_all = ids_missing;
            if (want_all) m->list_all();
            m->id_table(&d.ids, &d.nums);
            d.decoded = m->decoded();
            // review finding 4 (ruling 2026-11-21 19:11): written once per build, when missing, always with every registry
            if (d.env.write_data && want_all) d.env.write_data(ids_rel, m->id_table_json().dump());
            if (need.raw.empty()) write_base_cache(d.env, family, *src, *inventory, base, d.ids, d.nums, d.decoded);
        }
        if (old_fn) old = old_fn(*m);
        for (const std::string& n : m->take_notes()) d.log(0, "registry: " + n);
    }
    // compose, then the saves check of restart fields (saves.h; the live back-end checks every live write over the value under
    // it): an edit the saves refuse is taken out before the boot back-end writes anything. A plan a rollback starts on is
    // checked on every field, so it is refused whole rather than per entry at the world's load.
    std::vector<Refusal> save_refused;
    auto compose_checked = [&](const std::vector<ModInput>& in, bool plan) {
        save_refused.clear();
        Result x = compose(d.schema, base, old, in);
        std::vector<FieldRef> drop;
        for (const auto& [ref, fo] : x.fields) {
            if (is_raw_field(ref.field) || !has_registry_row(fo)) continue;
            const RegistrySpec* rs = d.schema.registry(ref.registry);
            const FieldSpec* f = rs ? rs->field(ref.field) : nullptr;
            if (!plan && !(f && f->applies == Applies::Restart)) continue;
            const auto v = saves::judge(ref.registry, ref.field, saves::before_registry(fo), fo.result);
            if (!v || v->decision != saves::Decision::Refuse) continue;
            for (const ChainRow& c : fo.chain)
                if (c.kind == WriterKind::Registry)
                    save_refused.push_back({c.mod, c.source, c.stage, c.n, "saves: " + ref.text() + " " + v->text, "", ref.registry, ref.id, ref.field});
            drop.push_back(ref);
        }
        for (const FieldRef& ref : drop) x.fields.erase(ref);
        return x;
    };
    auto stacks_lowered = [&](const Result& x) {
        std::vector<std::pair<FieldRef, std::pair<Value, Value>>> out;
        for (auto it = then.begin(); it != then.end(); ++it) {
            FieldRef ref;
            if (!field_ref_of(it.key(), &ref) || ref.registry != "items" || ref.field != "maxStack" || !it.value().is_number()) continue;
            Value now;
            if (const auto f = x.fields.find(ref); f != x.fields.end()) now = f->second.result;
            else if (const auto reg = base.registries.find("items"); reg != base.registries.end())
                for (const BaseModel::Entry& e : reg->second)
                    if (e.id == ref.id && e.fields.count("maxStack")) now = e.fields.at("maxStack");
            if (now.numeric() && now.as_double() < it.value().get<double>()) out.push_back({ref, {Value::number(it.value().get<double>()), now}});
        }
        return out;
    };
    Result r;
    if (src) r = compose_checked(inputs, rolled);
    std::string guard_why;
    bool guard_failed = false;
    if (rolled && src && r.refused.empty() && save_refused.empty()) {
        const auto lowered = stacks_lowered(r);
        if (!lowered.empty() && !(d.env.stack_guard && d.env.stack_guard(&guard_why))) {
            guard_failed = true;
            save_refused.push_back({"", "", Stage::Alter, 0, "saves: it lowers the largest stack of " + std::to_string(lowered.size()) + " item(s) (" +
                lowered.front().first.text() + " " + lowered.front().second.first.text_of() + " -> " + lowered.front().second.second.text_of() +
                ") below what a world already started with, and Kiln's stack guard is not available here" + (guard_why.empty() ? "" : " (" + guard_why + ")") +
                ": the game would delete the part of a saved stack above it", ""});
        }
    }
    if (rolled && (!src || !r.refused.empty() || !save_refused.empty())) {
        // never half-applied: one refused edit of the plan on this start refuses the whole plan
        std::string why = "the stage has no base to read";
        if (src && guard_failed) {
            why = save_refused.front().reason;
        } else if (src) {
            std::vector<Refusal> all = r.refused;
            all.insert(all.end(), save_refused.begin(), save_refused.end());
            const Refusal& x = all.front();
            why = "the plan's edit " + x.mod + " " + (x.source.empty() ? std::string("(no source)") : x.source) + " #" + std::to_string(x.n) +
                  " is refused on this start: " + x.reason;
            if (all.size() > 1) why += " (and " + std::to_string(all.size() - 1) + " more)";
        }
        rb["state"] = "refused";
        rb["reason"] = "plan " + short_hash(rb.value("plan", std::string())) + ": " + why + "; nothing of the plan was applied, this start runs on the mods' current registry edits";
        rb.erase("leftOut");
        inputs = std::move(fallback);
        fallback.clear();
        rolled = false;
        if (src) r = compose_checked(inputs, false);
    }
    // a start (or rollback) that gives items a lower max stack than a world already started with: the stack guard keeps the
    // bigger stacks a save holds whole; without it the save loses them at their next move (warned: nothing here can be refused)
    if (src) {
        const auto lowered = stacks_lowered(r);
        std::string w;
        const bool guarded = !lowered.empty() && d.env.stack_guard && d.env.stack_guard(&w);
        for (const auto& [ref, vals] : lowered) {
            const std::string a = vals.first.text_of(), b = vals.second.text_of();
            d.stage_notes.push_back({ref, "stackCut", guarded ? "guard" : "warn", guarded ?
                "a world already started with the largest stack " + a + ", this start gives " + b + ": Kiln's stack guard keeps the bigger stacks a save holds whole" :
                "a world already started with the largest stack " + a + ", this start gives " + b + ", and Kiln's stack guard is not available here" +
                    (w.empty() ? "" : " (" + w + ")") + ": the game deletes the part of a saved stack above " + b + " at its next move"});
        }
    }
    if (rolled) {
        // exactly what changed back: every value of the plan rolled away from, and of this start, that differs
        std::map<std::string, json> now;
        for (const auto& [ref, fo] : r.fields)
            if (has_registry_row(fo)) now[ref.text()] = fo.result.to_json();
        auto without_registry = [&](const std::string& key) -> json {
            FieldRef ref;
            if (!field_ref_of(key, &ref)) return nullptr;
            if (const auto it = r.fields.find(ref); it != r.fields.end()) return it->second.result.to_json();
            if (const auto reg = base.registries.find(ref.registry); reg != base.registries.end())
                for (const BaseModel::Entry& e : reg->second)
                    if (e.id == ref.id) {
                        const auto f = e.fields.find(ref.field);
                        return f == e.fields.end() ? json(nullptr) : f->second.to_json();
                    }
            return nullptr;
        };
        std::set<std::string> keys;
        for (auto it = from_values.begin(); it != from_values.end(); ++it) keys.insert(it.key());
        for (const auto& [k, v] : now) keys.insert(k);
        json changed = json::array();
        size_t total = 0;
        for (const std::string& k : keys) {
            const json a = from_values.contains(k) ? from_values[k] : without_registry(k);
            const auto n = now.find(k);
            const json b = n != now.end() ? n->second : without_registry(k);
            if (a == b) continue;
            if (++total <= 2000) changed.push_back({{"field", k}, {"from", a}, {"to", b}});
        }
        rb["state"] = "applied";
        rb["changedBack"] = std::move(changed);
        rb["changedBackTotal"] = total;
        if (from_values.empty() && rb["from"].is_string()) rb["fromValues"] = "the plan rolled away from is not in the plans folder: changedBack lists this start's values only";
        const std::string plan = rb.value("plan", std::string());
        d.log(1, "registry: rolled back to plan " + short_hash(plan) + " (kiln/data/content/registry/rollback.json): " + std::to_string(total) +
                     " value(s) changed back; saves are not touched; kilnctl registry rollback --clear returns to the mods' current edits");
        size_t shown = 0;
        for (const json& row : rb["changedBack"]) {
            if (++shown > 20) break;
            d.log(0, "registry: rollback: " + row["field"].get<std::string>() + " " + (row["from"].is_null() ? std::string("(none)") : row["from"].dump()) + " -> " +
                         (row["to"].is_null() ? std::string("(none)") : row["to"].dump()));
        }
        for (const json& m : rb["leftOut"]) d.log(1, "registry: rollback: mod " + m.get<std::string>() + "'s registry edits are left out (the plan does not hold them)");
    } else if (rb.is_object()) {
        d.log(1, "registry: the rollback " + (rb["plan"].is_string() ? "to plan " + short_hash(rb["plan"].get<std::string>()) + " " : std::string()) +
                     "is refused: " + rb.value("reason", std::string()));
    }
    for (const ModInput& in : inputs) {
        json edits = json::array();
        for (const Batch& b : in.batches) {
            json eb = {{"stage", stage_name(b.stage)}, {"source", b.source}, {"n", b.edits.size()}};
            edits.push_back(eb);
        }
        d.meta.order.push_back({in.id, versions[in.id], sha_hex(edits.dump()).substr(0, 16)});
    }
    // the schemas' id migrations: what they renamed in the edits this start runs (ir.cpp parse_batch did the renaming)
    json migrated = json::array();
    size_t migrated_logged = 0;
    for (const ModInput& in : inputs)
        for (const Batch& b : in.batches)
            for (const std::string& note : b.migrated) {
                if (migrated.size() < 2000) migrated.push_back({{"mod", in.id}, {"source", b.source}, {"stage", stage_name(b.stage)}, {"note", note}});
                if (++migrated_logged <= 50) d.log(0, "registry: migrated " + in.id + " " + (b.source.empty() ? std::string("(no source)") : b.source) + " " + note);
            }
    if (migrated_logged > 50) d.log(0, "registry: " + std::to_string(migrated_logged - 50) + " more migrated name(s): state/registry.json \"migrated\" lists them");
    // the edits refused before the stage are the report's too (and the saves check's: kept across a reload like them)
    std::vector<Refusal> pre;
    for (const ModInput& in : inputs) pre.insert(pre.end(), in.refused.begin(), in.refused.end());
    pre.insert(pre.end(), save_refused.begin(), save_refused.end());
    {
        std::lock_guard lk(d.mu);
        pre.insert(pre.end(), d.pre_refused.begin(), d.pre_refused.end());
    }
    r.refused.insert(r.refused.begin(), pre.begin(), pre.end());
    {
        std::lock_guard lk(d.mu);
        d.outside = pre;
    }
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
    // A cache hit has not populated the model's entry locations yet. Locate boot targets before releasing its source.
    if (m) m->release();
    std::lock_guard lk(d.mu);
    d.meta.migrated = std::move(migrated);
    d.meta.rollback = rb;
    if (rolled) {
        d.rollback = rb.value("plan", std::string());
        d.rollback_previous = rb.contains("previous") && rb["previous"].is_string() ? rb["previous"].get<std::string>() : std::string();
        rb.erase("previous");
        d.meta.rollback = rb;
    } else if (rb.is_object()) {
        d.meta.rollback.erase("previous");
    }
    d.full_live = eager && need.all && bool(src);
    d.base = std::move(base);
    d.old = std::move(old);
    for (ModInput& in : inputs) {
        in.code = nullptr;
        if (auto it = d.recorded.find(in.id); it != d.recorded.end())
            in.batches.insert(in.batches.end(), it->second.begin(), it->second.end());
    }
    d.recorded.clear();
    d.inputs = std::move(inputs);
    d.result = std::move(r);
    d.boot = boot;
    d.counts.refused = static_cast<uint32_t>(d.result.refused.size());
    d.counts.conflicts = static_cast<uint32_t>(d.result.lines.size());
    uint32_t edits = 0;
    for (const auto& [ref, fo] : d.result.fields)
        for (const ChainRow& c : fo.chain) edits += c.kind == WriterKind::Registry;
    d.counts.edits = edits + static_cast<uint32_t>(d.result.added.size() + d.result.removed.size());
    return boot;
}

bool Runner::prepare_live(LiveData* out, std::string* why) {
    std::lock_guard lock(d_->mu);
    auto& d = *d_;
    if (phase_.load() < Phase::Start || !d.env.prepare_live) { *why = "the registry stage is not ready"; return false; }
    try {
        if (!d.full_live) {
            if (d.installing_live) { *why = "the registry live handoff is still in progress"; return false; }
            if (!d.inputs.empty()) { *why = "the initial registry base failed; a restart is required"; return false; }
            if (!d.lazy_error.empty()) { *why = d.lazy_error; return false; }
            if (!d.lazy_source && d.env.source) d.lazy_source = d.env.source(why);
            if (!d.lazy_source) return false;
            std::unique_ptr<model::Source> consumed; // destroyed after the model and inventory below
            {
                auto& source = *d.lazy_source;
                const std::string origin = source.name();
                Inventory inventory(source);
                model::Model model(inventory, d.rm, d.schema);
                model::Need need; need.all = true;
                const auto family = cache_family(d.env, d.rm, need);
                LiveData data;
                if (!read_base_cache(d.env, family, source, &data)) {
                    model.fill(need, &data.base);
                    model.id_table(&data.ids, &data.nums);
                    data.decoded = model.decoded();
                    write_base_cache(d.env, family, source, inventory, data.base, data.ids, data.nums, data.decoded);
                }
                data.old = d.lazy_old ? d.lazy_old(model) : d.old;
                data.result = compose(d.schema, data.base, data.old, d.inputs);
                data.result.refused.insert(data.result.refused.begin(), d.outside.begin(), d.outside.end());
                // Stable live data must not carry the model's raw resolver beyond this owned read session.
                data.base.raw = {};
                for (const auto& note : model.take_notes()) d.log(0, "registry: " + note);
                d.base = std::move(data.base); d.old = std::move(data.old);
                d.live_result = std::move(data.result); d.result = d.live_result;
                d.ids = std::move(data.ids); d.nums = std::move(data.nums); d.decoded = std::move(data.decoded);
                d.meta.base.origin = origin;
                model.release();
                d.full_live = true; d.lazy_old = {};
                consumed = std::move(d.lazy_source);
            }
        }
        if (out) *out = {d.base, d.old, d.live_result, d.ids, d.nums, d.decoded};
        return true;
    } catch (const std::exception& error) {
        *why = std::string("the copied registry base could not be prepared: ") + error.what();
        return false;
    }
}

void Runner::finish(const std::vector<BootOutcome>& outcomes) {
    Impl& d = *d_;
    std::vector<uint64_t> ready;
    LiveHandoff live;
    uint64_t gen = 0;
    std::string plan_rel, plan_text, report_text;  // written after the runner's lock is released (review finding 10)
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
                {
                    const Refusal x{c.mod, c.source, c.stage, c.n, o.field.registry + " " + o.field.id + ": " + o.field.field + ": " + o.why + "; this edit was not applied", ""};
                    d.result.refused.push_back(x);
                    d.outside.push_back(x);
                }
            d.result.fields.erase(fo);  // kiln.log names the refusal (report_log)
        }
        d.counts.refused = static_cast<uint32_t>(d.result.refused.size());
        d.boot_fields.clear();
        for (const auto& [ref, fo] : d.result.fields) {
            const RegistrySpec* rs = d.schema.registry(ref.registry);
            const FieldSpec* f = rs && !is_raw_field(ref.field) ? rs->field(ref.field) : nullptr;
            if (f && f->applies == Applies::Restart) d.boot_fields[ref] = fo;
        }
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
        json removed = json::array();
        for (const EntryRef& e : d.result.removed) removed.push_back({{"registry", e.registry}, {"id", e.id}});
        json key = {{"exeSha256", d.env.exe_sha256}, {"side", d.env.side}, {"kiln", d.env.kiln_version}, {"decoder", model::kDecoderVersion},
                    {"regmapRecord", d.meta.regmap.record}, {"order", json::array()}};
        for (const auto& o : d.meta.order) key["order"].push_back({{"mod", o.mod}, {"version", o.version}, {"inputs", o.edits_hash}});
        // registry-rollback: the plan keeps what a later start needs to run on it again: each mod's lowered edits (data
        // sections, queued batches and recorded callback turns, in the order they composed) and the values they resolved to
        json inputs = json::array();
        for (const ModInput& in : d.inputs) {
            std::string version;
            for (const auto& o : d.meta.order)
                if (o.mod == in.id) version = o.version;
            json batches = json::array();
            for (const Batch& b : in.batches) batches.push_back(batch_json(b));
            inputs.push_back({{"mod", in.id}, {"version", version}, {"batches", std::move(batches)}});
        }
        json values = json::object();
        for (const auto& [ref, fo] : d.result.fields)
            if (has_registry_row(fo)) values[ref.text()] = fo.result.to_json();
        json plan = {{"format", 2}, {"key", key}, {"boot", bootj}, {"live", livej}, {"adds", adds}, {"removed", removed}, {"values", values}, {"inputs", inputs}};
        const std::string hash = d.rollback.empty() ? plan_hash_of(plan) : d.rollback;
        // previous: the last good plan that is not this one (a world started on it without a refused edit, plans/index.json)
        std::string previous = d.rollback_previous;
        if (d.rollback.empty()) {
            const json good = d.index.contains("good") && d.index["good"].is_object() ? d.index["good"] : json::object();
            const std::string g = good.contains("plan") && good["plan"].is_string() ? good["plan"].get<std::string>() : "";
            const std::string gp = good.contains("previous") && good["previous"].is_string() ? good["previous"].get<std::string>() : "";
            previous = g != hash ? g : gp;
        }
        if (d.env.write_data) {
            // a rolled-back start runs on a plan that is already written (and named by its own content): it is not rewritten
            if (d.rollback.empty()) {
                plan["previous"] = previous.empty() ? json(nullptr) : json(previous);
                plan_rel = "plans/" + hash + ".json", plan_text = plan.dump(1);
            }
            d.meta.plan = {hash, "kiln/data/content/registry/plans/" + hash + ".json", previous};
        }
        d.meta.saves = d.saves_json(d.result);
        const json doc = ::kc::registry::report_json(d.schema, d.result, d.meta);
        d.report = doc.dump();
        report_text = doc.dump(1);
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
        for (const auto& [h, s] : d.ready_subs) ready.push_back(h);
        live = {&d.schema, &d.rm, &d.result, &d.ids, &d.nums, &d.base, &d.old, &d.inputs, &d.decoded, nullptr, &d.outside, &d.boot_fields, nullptr};
        live.rollback = d.rollback;
        live.check = [this](Batch* b, std::vector<Refusal>* refused) {
            std::lock_guard l2(d_->mu);
            refuse_unmapped(d_->rm, d_->schema, b, refused, d_->env.entry_support);
        };
        auto publish = [this](const Result& r, bool application, bool world_event, bool world) {
            std::vector<uint64_t> handles;
            uint64_t g2 = 0;
            std::string text, index_text;
            {
                std::lock_guard l2(d_->mu);
                // registry-rollback: a world started on this plan; without a refused edit it is the last good plan, and the good
                // one before it stays its "previous" (plans/index.json)
                if (world_event && !d_->meta.plan.hash.empty() && d_->env.write_data) {
                    json& idx = d_->index;
                    const std::string at = d_->env.now ? d_->env.now() : "";
                    idx["format"] = 1;
                    idx["world"] = {{"plan", d_->meta.plan.hash}, {"at", at}};
                    // the highest max stack each item a registry edit touches had in a world (saves.h stackCut): never lowered
                    if (!idx.contains("stacks") || !idx["stacks"].is_object()) idx["stacks"] = json::object();
                    for (const auto& [ref, fo] : r.fields) {
                        if (ref.registry != "items" || ref.field != "maxStack" || !has_registry_row(fo) || !fo.result.numeric()) continue;
                        json& top = idx["stacks"][ref.text()];
                        if (!top.is_number() || top.get<double>() < fo.result.as_double()) top = fo.result.to_json();
                    }
                    if (r.refused.empty()) {
                        const json good = idx.contains("good") && idx["good"].is_object() ? idx["good"] : json::object();
                        const std::string g = good.contains("plan") && good["plan"].is_string() ? good["plan"].get<std::string>() : "";
                        if (g != d_->meta.plan.hash) idx["good"] = {{"plan", d_->meta.plan.hash}, {"previous", g.empty() ? json(nullptr) : json(g)}, {"at", at}};
                        else idx["good"]["at"] = at;
                    }
                    index_text = idx.dump(1);
                }
                d_->result = r;
                d_->counts.refused = static_cast<uint32_t>(r.refused.size());
                d_->counts.conflicts = static_cast<uint32_t>(r.lines.size());
                d_->counts.edits = static_cast<uint32_t>(r.added.size() + r.removed.size());
                for (const auto& [ref, field] : r.fields)
                    for (const auto& row : field.chain) d_->counts.edits += row.kind == WriterKind::Registry;
                if (application) {
                    d_->meta.generation = ++d_->generation;
                    d_->meta.world_load = world;
                    g2 = d_->generation;
                }
                if (world_event) {
                    phase_.store(Phase::WorldLoad);
                    for (const auto& [h, s] : d_->ready_subs) handles.push_back(h);
                }
                d_->meta.written = d_->env.now ? d_->env.now() : "";
                d_->meta.saves = d_->saves_json(d_->result);
                const json doc2 = ::kc::registry::report_json(d_->schema, d_->result, d_->meta);
                d_->report = doc2.dump();
                text = doc2.dump(1);
            }
            // Outside the runner lock: transaction snapshots update the generation/file, never the ready event.
            if (!index_text.empty() && !d_->env.write_data("plans/index.json", index_text)) d_->log(1, "registry: kiln/data/content/registry/plans/index.json could not be written");
            if (d_->env.write_report) d_->env.write_report(text);
            notify_ready(handles, Phase::WorldLoad, g2);
        };
        live.report = [publish](const Result& r, bool application) { publish(r, application, application, application); };
        live.changed = [publish](const Result& r, bool world) { publish(r, true, false, world); };
        if (d.env.prepare_live && !d.full_live) live.prepare = [this](LiveData* data, std::string* why) { return prepare_live(data, why); };
    }
    if (!plan_rel.empty() && !d.env.write_data(plan_rel, plan_text)) d.log(1, "registry: the plan could not be written");
    if (d.env.write_report && !d.env.write_report(report_text)) d.log(1, "registry: state/registry.json could not be written");
    // The installer copies borrowed handoff maps outside this lock (store -> runner is the lock order).
    // Fail closed for polling readers until that copy completes; preparation must not mutate those maps concurrently.
    { std::lock_guard lock(d.mu); d.installing_live = true; }
    auto complete_handoff = [&d] { std::lock_guard lock(d.mu); d.installing_live = false; };
    try {
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
    } catch (...) { complete_handoff(); throw; }
    complete_handoff();
    notify_ready(ready, Phase::Start, gen);
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
    if (view != 1 && phase_.load() >= Phase::Start && d_->env.prepare_live) {
        std::string why; if (!prepare_live(nullptr, &why)) return KILN_ERR_UNAVAILABLE;
    }
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
        if (const auto entry = d_->result.definitions.find({registry, ref.id}); entry != d_->result.definitions.end()) {
            const auto value = entry->second.values.find(field);
            if (value == entry->second.values.end()) return KILN_ERR_UNAVAILABLE;
            return *out = value->second.to_json().dump(), KILN_OK;
        }
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
    if (view != 1 && phase_.load() >= Phase::Start && d_->env.prepare_live) {
        std::string why; if (!prepare_live(nullptr, &why)) return KILN_ERR_UNAVAILABLE;
    }
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
            if (a.registry == registry && !removed.count(a.id)) {
                const auto definition = d_->result.definitions.find(a);
                take(a.id, definition == d_->result.definitions.end() ? std::set<std::string>{} : definition->second.traits);
            }
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
