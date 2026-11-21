// SPDX-License-Identifier: MIT
// kiln.stationmenus: the store of station menu layouts and the crafting-menu trees built from them. menulayout.h says what
// is here; the ABI is native/core/abi/services/kiln_station_menus.h; the record's shape is dev31 DESIGN.md section 2.
#include "menulayout.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <set>

#include "nlohmann/json.hpp"
#include "services/kiln_caps.h"

namespace kc::ml {
namespace {

using json = nlohmann::json;  // objects are std::map: keys in ordinal byte order, the canonical form

uint32_t fnv32(const uint8_t* p, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}
uint64_t fnv64(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

// A text for a message: at most 64 bytes of it, never cut inside a UTF-8 sequence.
std::string shown(const std::string& s) {
    if (s.size() <= 64) return s;
    size_t n = 64;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "...";
}

// ---- grammar ----
bool key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}
// a key of a tab, a set, a definition, a station, a recipe: [A-Za-z0-9._-]{1,64}, case-sensitive
bool valid_key(std::string_view s) { return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), key_char); }
// a mod id: [a-z0-9._-]{1,64}; mod ids compare case-insensitively, so upper-case letters are the same id (kept as written)
bool valid_mod(std::string_view s) { return valid_key(s); }
// "mod:key"
bool valid_recipe_key(std::string_view s) {
    const size_t c = s.find(':');
    return c != std::string_view::npos && valid_mod(s.substr(0, c)) && valid_key(s.substr(c + 1));
}
// "*" or a lower-case language tag: [a-z]{2,3}(-[a-z0-9]{2,8})*
bool valid_language(std::string_view k) {
    if (k == "*") return true;
    size_t i = 0;
    while (i < k.size() && k[i] >= 'a' && k[i] <= 'z') ++i;
    if (i < 2 || i > 3) return false;
    while (i < k.size()) {
        if (k[i] != '-') return false;
        size_t j = ++i;
        while (j < k.size() && ((k[j] >= 'a' && k[j] <= 'z') || (k[j] >= '0' && k[j] <= '9'))) ++j;
        if (j - i < 2 || j - i > 8) return false;
        i = j;
    }
    return true;
}
// lower-case 8-4-4-4-12 -> the game's GUID bytes (data1, data2, data3 little-endian; as guid_parse and .NET's
// Guid.TryWriteBytes, so that FNV-1a over them is the game's id). The nil GUID is refused.
bool parse_guid(const std::string& s, uint8_t out[16]) {
    if (s.size() != 36) return false;
    uint8_t raw[16];
    int n = 0;
    for (size_t i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
            ++i;
            continue;
        }
        const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        const int a = hex(s[i]), b = hex(s[i + 1]);
        if (a < 0 || b < 0) return false;
        raw[n++] = static_cast<uint8_t>(a * 16 + b);
        i += 2;
    }
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    bool any = false;
    for (int i = 0; i < 16; ++i) {
        out[i] = raw[order[i]];
        any = any || out[i] != 0;
    }
    return any;
}

// ---- the parser ----
// First pass over the text: a member twice in one object (a parser keeps one of them silently) and nesting deeper than any
// record has are refused before the document is built.
struct Strict final : nlohmann::json_sax<json> {
    std::vector<std::set<std::string>> keys;
    std::string error;
    size_t depth = 0;
    bool open() {
        if (++depth <= 16) return true;
        error = "nested deeper than a station menu record is";
        return false;
    }
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool start_object(std::size_t) override {
        keys.emplace_back();
        return open();
    }
    bool key(string_t& k) override {
        if (keys.back().insert(k).second) return true;
        error = "member \"" + shown(k) + "\" appears twice in one object";
        return false;
    }
    bool end_object() override {
        keys.pop_back();
        --depth;
        return true;
    }
    bool start_array(std::size_t) override { return open(); }
    bool end_array() override {
        --depth;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& e) override {
        error = std::string("not JSON (") + e.what() + ")";
        return false;
    }
};
bool read_json(std::string_view text, json* out, std::string* error) {
    Strict strict;
    if (!json::sax_parse(text.begin(), text.end(), &strict) || !strict.error.empty()) {
        *error = strict.error.empty() ? "not JSON" : strict.error;
        return false;
    }
    try {
        *out = json::parse(text.begin(), text.end());  // UTF-8 is checked here: a string with a bad sequence is not JSON
    } catch (const std::exception& e) {
        *error = std::string("not JSON (") + e.what() + ")";
        return false;
    }
    return true;
}

bool fail(std::string* e, std::string m) {
    *e = std::move(m);
    return false;
}
// exactly these members, each of them
bool members(const json& o, std::initializer_list<const char*> want, const std::string& where, std::string* e) {
    if (!o.is_object()) return fail(e, where + " must be an object");
    for (auto it = o.begin(); it != o.end(); ++it)
        if (std::none_of(want.begin(), want.end(), [&](const char* w) { return it.key() == w; }))
            return fail(e, where + " has an unknown member \"" + shown(it.key()) + "\"");
    for (const char* w : want)
        if (!o.contains(w)) return fail(e, where + " lacks \"" + w + "\"");
    return true;
}
bool u32_of(const json& v, uint32_t* out) {
    if (!v.is_number_unsigned() || v.get<uint64_t>() > UINT32_MAX) return false;
    *out = static_cast<uint32_t>(v.get<uint64_t>());
    return true;
}
bool string_of(const json& v, std::string* out) {
    if (!v.is_string()) return false;
    *out = v.get<std::string>();
    return true;
}
bool guid_of(const json& v, const std::string& where, std::string* text, uint8_t out[16], std::string* e) {
    if (!string_of(v, text) || !parse_guid(*text, out))
        return fail(e, where + " must be a GUID, lower-case xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, not the nil GUID");
    return true;
}
bool texts_of(const json& v, const std::string& where, TextMap* out, std::string* e) {
    if (!v.is_object()) return fail(e, where + " must be a language map {\"en\": text, ...}");
    if (v.empty() || v.size() > kMaxLanguages) return fail(e, where + " holds 1 to 35 languages, not " + std::to_string(v.size()));
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (!valid_language(it.key())) return fail(e, where + ": \"" + shown(it.key()) + "\" is not \"*\" or a lower-case language tag (en, pt-br)");
        if (!it.value().is_string()) return fail(e, where + "[\"" + it.key() + "\"] must be a text");
        const std::string& s = it.value().get_ref<const std::string&>();
        if (s.empty() || s.size() > kMaxTextBytes)
            return fail(e, where + "[\"" + it.key() + "\"] holds 1 to 4096 bytes, not " + std::to_string(s.size()));
        (*out)[it.key()] = s;
    }
    return true;
}

// One record: every check of the format, and the canonical bytes.
bool record_from(const json& j, Record* r, std::string* e) {
    if (!members(j, {"baseWorkshopId", "definition", "format", "origin", "tabs"}, "the record", e)) return false;
    if (!j["format"].is_number_unsigned() || j["format"].get<uint64_t>() != 1)
        return fail(e, "unknown format " + shown(j["format"].dump()) + ": this Kiln reads station menu records of format 1");
    const json& d = j["definition"];
    if (!members(d, {"id", "key", "mod"}, "definition", e)) return false;
    if (!string_of(d["mod"], &r->def_mod) || !valid_mod(r->def_mod)) return fail(e, "definition.mod must be a mod id ([a-z0-9._-], 1 to 64)");
    if (!string_of(d["key"], &r->def_key) || !valid_key(r->def_key)) return fail(e, "definition.key must be a key ([A-Za-z0-9._-], 1 to 64)");
    if (!guid_of(d["id"], "definition.id", &r->def_id_text, r->def_id, e)) return false;
    const json& o = j["origin"];
    if (!members(o, {"mod", "stationKey", "workshopGuid", "workshopId"}, "origin", e)) return false;
    if (!string_of(o["mod"], &r->origin_mod) || !valid_mod(r->origin_mod)) return fail(e, "origin.mod must be a mod id ([a-z0-9._-], 1 to 64)");
    if (!string_of(o["stationKey"], &r->station_key) || !valid_key(r->station_key))
        return fail(e, "origin.stationKey must be a key ([A-Za-z0-9._-], 1 to 64)");
    if (!guid_of(o["workshopGuid"], "origin.workshopGuid", &r->workshop_guid_text, r->workshop_guid, e)) return false;
    if (!u32_of(o["workshopId"], &r->workshop)) return fail(e, "origin.workshopId must be an unsigned 32-bit number");
    if (r->workshop != fnv32(r->workshop_guid, 16))
        return fail(e, "origin.workshopId " + std::to_string(r->workshop) + " is not the id of workshop " + r->workshop_guid_text + " (" +
                           std::to_string(fnv32(r->workshop_guid, 16)) + ")");
    if (!u32_of(j["baseWorkshopId"], &r->base)) return fail(e, "baseWorkshopId must be an unsigned 32-bit number");
    const json& tabs = j["tabs"];
    if (!tabs.is_array()) return fail(e, "tabs must be an array");
    if (tabs.size() > kMaxTabs) return fail(e, "a menu holds at most 15 tabs, not " + std::to_string(tabs.size()));
    std::set<std::string> tab_keys, ids;
    std::set<uint32_t> recipes;
    for (size_t ti = 0; ti < tabs.size(); ++ti) {
        const json& t = tabs[ti];
        const std::string tw = "tabs[" + std::to_string(ti) + "]";
        if (!members(t, {"iconFromTab", "id", "key", "name", "sets"}, tw, e)) return false;
        MenuTab tab;
        if (!string_of(t["key"], &tab.key) || !valid_key(tab.key)) return fail(e, tw + ".key must be a key ([A-Za-z0-9._-], 1 to 64)");
        if (!tab_keys.insert(tab.key).second) return fail(e, "tab key \"" + tab.key + "\" appears twice");
        if (!guid_of(t["id"], tw + ".id", &tab.id_text, tab.id, e)) return false;
        if (!ids.insert(tab.id_text).second) return fail(e, tw + ".id " + tab.id_text + " is the id of another tab or set");
        const json& icon = t["iconFromTab"];
        if (!icon.is_number_integer() || icon.get<int64_t>() < 0 || icon.get<int64_t>() >= static_cast<int64_t>(kMaxTabs))
            return fail(e, tw + ".iconFromTab must be a number 0 to 14 (the tab of the base menu whose icon it shows)");
        tab.icon = static_cast<uint32_t>(icon.get<int64_t>());
        if (!texts_of(t["name"], tw + ".name", &tab.name, e)) return false;
        tab.name_json = t["name"].dump(-1, ' ', false, json::error_handler_t::strict);
        const json& sets = t["sets"];
        if (!sets.is_array()) return fail(e, tw + ".sets must be an array");
        if (sets.size() > kMaxSetsPerTab) return fail(e, tw + " holds at most 28 sets, not " + std::to_string(sets.size()));
        std::set<std::string> set_keys;
        size_t in_tab = 0;
        for (size_t si = 0; si < sets.size(); ++si) {
            const json& s = sets[si];
            const std::string sw = tw + ".sets[" + std::to_string(si) + "]";
            if (!members(s, {"heading", "id", "key", "recipes"}, sw, e)) return false;
            MenuSet set;
            if (!string_of(s["key"], &set.key) || !valid_key(set.key)) return fail(e, sw + ".key must be a key ([A-Za-z0-9._-], 1 to 64)");
            if (!set_keys.insert(set.key).second) return fail(e, "set key \"" + set.key + "\" appears twice in tab \"" + tab.key + "\"");
            if (!guid_of(s["id"], sw + ".id", &set.id_text, set.id, e)) return false;
            if (!ids.insert(set.id_text).second) return fail(e, sw + ".id " + set.id_text + " is the id of another tab or set");
            if (!s["heading"].is_null()) {
                if (si == 0) return fail(e, sw + ": the first set of a tab has no heading (heading: null)");
                if (!texts_of(s["heading"], sw + ".heading", &set.heading, e)) return false;
                set.has_heading = true;
                set.heading_json = s["heading"].dump(-1, ' ', false, json::error_handler_t::strict);
            }
            const json& rs = s["recipes"];
            if (!rs.is_array()) return fail(e, sw + ".recipes must be an array");
            in_tab += rs.size();
            if (in_tab > kMaxRecipesPerTab) return fail(e, "tab \"" + tab.key + "\" lists more than 512 recipes");
            for (size_t ri = 0; ri < rs.size(); ++ri) {
                const json& rr = rs[ri];
                const std::string rw = sw + ".recipes[" + std::to_string(ri) + "]";
                if (!members(rr, {"key", "recipeId"}, rw, e)) return false;
                std::string key;
                if (!string_of(rr["key"], &key) || !valid_recipe_key(key)) return fail(e, rw + ".key must be \"mod:key\"");
                uint32_t id = 0;
                if (!u32_of(rr["recipeId"], &id) || !id) return fail(e, rw + ".recipeId must be an unsigned 32-bit number other than 0");
                if (!recipes.insert(id).second) return fail(e, "recipe " + std::to_string(id) + " (" + shown(key) + ") is listed twice");
                set.recipes.push_back(id);
            }
            tab.sets.push_back(std::move(set));
        }
        r->tabs.push_back(std::move(tab));
    }
    r->canonical = j.dump(-1, ' ', false, json::error_handler_t::strict);
    if (r->canonical.size() > kMaxRecordBytes) return fail(e, "the record is larger than 1 MiB");
    r->hash = fnv64(r->canonical);
    return true;
}

}  // namespace

bool parse_record(std::string_view text, Record* out, std::string* error) {
    *out = Record{};
    if (text.size() > kMaxRecordBytes) return fail(error, "the record is larger than 1 MiB");
    json j;
    if (!read_json(text, &j, error)) return false;
    try {
        return record_from(j, out, error);
    } catch (const std::exception& e) {
        return fail(error, std::string("the record could not be read: ") + e.what());
    }
}

// ============================================================================================= the store ==========
namespace {
struct Layer {
    KilnOwner owner = 0;
    KilnHandle handle = 0;
    uint64_t seq = 0;         // accept order: the newest of a workshop is effective
    uint64_t generation = 0;  // the local generation it was accepted at
    std::shared_ptr<const Record> rec;
};
struct Remote {
    KilnOwner owner = 0;
    uint64_t generation = 0;
    std::string identity;  // its records' canonical bytes in order, with the generation: an identical snapshot is a no-op
    std::map<uint32_t, std::shared_ptr<const Record>> by_workshop;
};
struct Store {
    std::mutex mu;
    std::map<KilnHandle, Layer> layers;
    KilnHandle next_handle = 1;
    uint64_t next_seq = 1;
    uint64_t generation = 0;
    size_t bytes = 0;
    bool has_remote = false;
    Remote remote;
    std::deque<Applied> applied;
    std::function<bool(KilnOwner)> revoked;
    std::atomic<bool (*)(uint32_t*, std::string*)> render{nullptr};
    std::atomic<uint64_t> version{1};
    std::atomic<bool> pending{false};
    std::atomic<uint32_t> materialized{0};
};
Store& S() {
    static Store s;
    return s;
}
bool revoked_now(KilnOwner o) {
    std::function<bool(KilnOwner)> fn;
    {
        std::lock_guard lock(S().mu);
        fn = S().revoked;
    }
    return fn && fn(o);
}
void bump_locked(Store& s) {
    if (s.generation != UINT64_MAX) ++s.generation;
    s.version.fetch_add(1);
}
std::map<uint32_t, Effective> effective_locked(const Store& s) {
    std::map<uint32_t, Effective> m;
    std::map<uint32_t, uint64_t> seq;
    for (const auto& [h, l] : s.layers) {
        const uint32_t w = l.rec->workshop;
        if (!m.count(w) || l.seq > seq[w]) {
            m[w] = Effective{w, l.rec, false, l.generation};
            seq[w] = l.seq;
        }
    }
    if (s.has_remote)
        for (const auto& [w, rec] : s.remote.by_workshop) m[w] = Effective{w, rec, true, s.remote.generation};
    return m;
}
void put_report(char* report, size_t cap, size_t* out_len, bool ok, const std::string& state, const std::string& error) {
    json j{{"ok", ok}, {"state", state}};
    if (!error.empty()) {
        std::string e = error.size() > 3000 ? error.substr(0, 3000) : error;
        while (!e.empty() && (static_cast<unsigned char>(e.back()) & 0x80)) e.pop_back();  // never a cut sequence
        j["error"] = e;
    }
    const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    if (out_len) *out_len = s.size();
    if (report && cap) {
        const size_t n = std::min(s.size(), cap - 1);
        std::memcpy(report, s.data(), n);
        report[n] = '\0';
    }
}
KilnResult put_text(const std::string& s, char* out, size_t cap, size_t* out_len) {
    if (out_len) *out_len = s.size();
    if (!out || cap < s.size() + 1) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return KILN_OK;
}
}  // namespace

void set_owner_probe(std::function<bool(KilnOwner)> revoked) {
    std::lock_guard lock(S().mu);
    S().revoked = std::move(revoked);
}
void set_render_probe(bool (*probe)(uint32_t*, std::string*)) { S().render.store(probe); }

void probe_owners() {
    Store& s = S();
    std::function<bool(KilnOwner)> fn;
    std::vector<KilnOwner> owners;
    {
        std::lock_guard lock(s.mu);
        fn = s.revoked;
        if (!fn) return;
        for (const auto& [h, l] : s.layers) owners.push_back(l.owner);
        if (s.has_remote) owners.push_back(s.remote.owner);
    }
    std::sort(owners.begin(), owners.end());
    owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
    std::set<KilnOwner> gone;
    for (KilnOwner o : owners)  // without the lock: the core's answer may take its own
        if (fn(o)) gone.insert(o);
    if (gone.empty()) return;
    std::lock_guard lock(s.mu);
    size_t dropped = 0;
    for (auto it = s.layers.begin(); it != s.layers.end();) {
        if (gone.count(it->second.owner)) {
            s.bytes -= it->second.rec->canonical.size();
            it = s.layers.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    if (dropped) bump_locked(s);
    if (s.has_remote && gone.count(s.remote.owner)) {
        s.has_remote = false;
        s.remote = Remote{};
        s.version.fetch_add(1);
    }
}

KilnResult define(KilnOwner owner, std::string_view text, KilnHandle* out, uint64_t* out_generation, std::string* state,
                  std::string* error) {
    Store& s = S();
    *state = "refused";
    if (out) *out = 0;
    if (!owner) return fail(error, "owner 0 is the core's: a layer belongs to a mod"), KILN_ERR_INVALID_ARGUMENT;
    probe_owners();
    if (revoked_now(owner)) {
        std::lock_guard lock(s.mu);
        if (out_generation) *out_generation = s.generation;
        *error = "the owner was revoked";
        return KILN_ERR_REVOKED;
    }
    auto rec = std::make_shared<Record>();
    if (!parse_record(text, rec.get(), error)) {
        std::lock_guard lock(s.mu);
        if (out_generation) *out_generation = s.generation;
        return KILN_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(s.mu);
    if (out_generation) *out_generation = s.generation;
    Layer* mine = nullptr;
    for (auto& [h, l] : s.layers)
        if (l.owner == owner && std::memcmp(l.rec->def_id, rec->def_id, 16) == 0) mine = &l;
    if (mine && mine->rec->canonical == rec->canonical) {
        if (out) *out = mine->handle;
        *state = "unchanged";
        return KILN_OK;
    }
    const size_t old = mine ? mine->rec->canonical.size() : 0;
    if (!mine && s.layers.size() >= kMaxLayers) return fail(error, "256 station menu layers are stored already"), KILN_ERR_INVALID_ARGUMENT;
    if (s.bytes - old + rec->canonical.size() > kMaxAllBytes)
        return fail(error, "the station menu layers would hold more than 64 MiB of records"), KILN_ERR_INVALID_ARGUMENT;
    if (s.generation == UINT64_MAX) return fail(error, "the station menus' generation counter is exhausted"), KILN_ERR_INVALID_ARGUMENT;
    ++s.generation;
    s.version.fetch_add(1);
    s.bytes = s.bytes - old + rec->canonical.size();
    if (mine) {
        mine->rec = std::move(rec);
        mine->seq = s.next_seq++;
        mine->generation = s.generation;
        *state = "replaced";
    } else {
        Layer l;
        l.owner = owner;
        l.handle = s.next_handle++;
        l.seq = s.next_seq++;
        l.generation = s.generation;
        l.rec = std::move(rec);
        mine = &(s.layers[l.handle] = std::move(l));
        *state = "defined";
    }
    if (out) *out = mine->handle;
    if (out_generation) *out_generation = s.generation;
    return KILN_OK;
}

KilnResult remove(KilnOwner caller, KilnHandle handle, uint64_t* out_generation) {
    probe_owners();
    Store& s = S();
    std::lock_guard lock(s.mu);
    if (out_generation) *out_generation = s.generation;
    const auto it = s.layers.find(handle);
    if (it == s.layers.end()) return KILN_ERR_NOT_FOUND;
    if (it->second.owner != caller) return KILN_ERR_INVALID_ARGUMENT;
    if (s.generation == UINT64_MAX) return KILN_ERR_INVALID_ARGUMENT;  // exhausted: nothing changes any more
    s.bytes -= it->second.rec->canonical.size();
    s.layers.erase(it);
    bump_locked(s);
    if (out_generation) *out_generation = s.generation;
    return KILN_OK;
}

std::string export_layouts() {
    probe_owners();
    Store& s = S();
    std::lock_guard lock(s.mu);
    std::map<uint32_t, std::pair<uint64_t, const Record*>> best;  // workshop -> the newest local layer
    for (const auto& [h, l] : s.layers) {
        auto& b = best[l.rec->workshop];
        if (!b.second || l.seq > b.first) b = {l.seq, l.rec.get()};
    }
    // the container as the serializer writes it: {"format":1,"generation":N,"menus":[...]} (keys sorted, compact)
    std::string out = "{\"format\":1,\"generation\":" + std::to_string(s.generation) + ",\"menus\":[";
    bool first = true;
    for (const auto& [w, b] : best) {
        if (!first) out += ',';
        out += b.second->canonical;
        first = false;
    }
    out += "]}";
    return out;
}

KilnResult remote_replace(KilnOwner owner, std::string_view text, std::string* state, std::string* error) {
    *state = "refused";
    if (!owner) return fail(error, "owner 0 is the core's: the remote collection belongs to the service's owner"), KILN_ERR_INVALID_ARGUMENT;
    probe_owners();
    if (revoked_now(owner)) return fail(error, "the owner was revoked"), KILN_ERR_REVOKED;
    if (text.size() > kMaxAllBytes) return fail(error, "the snapshot is larger than 64 MiB"), KILN_ERR_INVALID_ARGUMENT;
    Remote r;
    r.owner = owner;
    try {
        json j;
        if (!read_json(text, &j, error)) return KILN_ERR_INVALID_ARGUMENT;
        if (!members(j, {"format", "generation", "menus"}, "the snapshot", error)) return KILN_ERR_INVALID_ARGUMENT;
        if (!j["format"].is_number_unsigned() || j["format"].get<uint64_t>() != 1)
            return fail(error, "unknown snapshot format: this Kiln reads format 1"), KILN_ERR_INVALID_ARGUMENT;
        if (!j["generation"].is_number_unsigned()) return fail(error, "the snapshot's generation must be an unsigned number"), KILN_ERR_INVALID_ARGUMENT;
        r.generation = j["generation"].get<uint64_t>();
        const json& menus = j["menus"];
        if (!menus.is_array()) return fail(error, "the snapshot's menus must be an array"), KILN_ERR_INVALID_ARGUMENT;
        if (menus.size() > kMaxRemoteMenus) return fail(error, "a snapshot holds at most 256 menus"), KILN_ERR_INVALID_ARGUMENT;
        std::set<std::string> defs;
        size_t all = 0;
        r.identity = std::to_string(r.generation);
        for (size_t i = 0; i < menus.size(); ++i) {
            auto rec = std::make_shared<Record>();
            std::string why;
            if (!record_from(menus[i], rec.get(), &why)) return fail(error, "menus[" + std::to_string(i) + "]: " + why), KILN_ERR_INVALID_ARGUMENT;
            if (!defs.insert(rec->def_id_text).second)
                return fail(error, "the snapshot holds definition " + rec->def_id_text + " twice"), KILN_ERR_INVALID_ARGUMENT;
            if (r.by_workshop.count(rec->workshop)) {
                char b[96];
                std::snprintf(b, sizeof b, "the snapshot holds two menus of workshop %u", rec->workshop);
                return fail(error, b), KILN_ERR_INVALID_ARGUMENT;
            }
            all += rec->canonical.size();
            if (all > kMaxAllBytes) return fail(error, "the snapshot's records are larger than 64 MiB"), KILN_ERR_INVALID_ARGUMENT;
            r.identity += '\n';
            r.identity += rec->canonical;
            r.by_workshop[rec->workshop] = std::move(rec);
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "out of memory reading the snapshot"), KILN_ERR_INTERNAL;
    } catch (const std::exception& e) {
        return fail(error, std::string("the snapshot could not be read: ") + e.what()), KILN_ERR_INVALID_ARGUMENT;
    }
    Store& s = S();
    std::lock_guard lock(s.mu);
    if (s.has_remote && s.remote.owner != owner)
        return fail(error, "another owner holds the remote collection"), KILN_ERR_INVALID_ARGUMENT;
    if (s.has_remote && s.remote.identity == r.identity) {
        *state = "unchanged";
        return KILN_OK;
    }
    s.remote = std::move(r);
    s.has_remote = true;
    s.version.fetch_add(1);
    *state = "replaced";
    return KILN_OK;
}

KilnResult remote_clear(KilnOwner owner) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    Store& s = S();
    std::lock_guard lock(s.mu);
    if (!s.has_remote) return KILN_OK;
    if (s.remote.owner != owner) return KILN_ERR_INVALID_ARGUMENT;
    s.has_remote = false;
    s.remote = Remote{};
    s.version.fetch_add(1);
    return KILN_OK;
}

void set_pending(bool on) { S().pending.store(on); }
bool pending() { return S().pending.load(); }

KilnResult info(KilnStationMenusInfo* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    probe_owners();
    Store& s = S();
    std::memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->store = 1;
    uint32_t reason = KILN_CAP_REASON_SYMBOL_MISSING;
    std::string detail = "kiln.content's crafting-menu hook is not on in this build (content.new / content.recipe_menu)";
    bool (*probe)(uint32_t*, std::string*) = s.render.load();
    const bool render = probe && probe(&reason, &detail);
    out->render = render ? 1 : 0;
    out->render_reason = render ? static_cast<uint32_t>(KILN_CAP_REASON_NONE) : reason;
    size_t n = std::min(detail.size(), sizeof out->detail - 1);
    while (n < detail.size() && n > 0 && (static_cast<unsigned char>(detail[n]) & 0xC0) == 0x80) --n;
    std::memcpy(out->detail, detail.data(), n);
    out->detail[n] = '\0';
    std::lock_guard lock(s.mu);
    out->generation = s.generation;
    out->remote_generation = s.has_remote ? s.remote.generation : 0;
    out->layers = static_cast<uint32_t>(s.layers.size());
    out->effective = static_cast<uint32_t>(effective_locked(s).size());
    out->remote = s.has_remote ? 1 : 0;
    out->pending = s.pending.load() ? 1 : 0;
    out->materialized = s.materialized.load();
    return KILN_OK;
}

std::vector<Effective> effective() {
    Store& s = S();
    std::lock_guard lock(s.mu);
    std::vector<Effective> v;
    for (auto& [w, e] : effective_locked(s)) v.push_back(std::move(e));
    return v;
}
bool has_layout(uint32_t workshop) {
    Store& s = S();
    std::lock_guard lock(s.mu);
    if (s.has_remote && s.remote.by_workshop.count(workshop)) return true;
    return std::any_of(s.layers.begin(), s.layers.end(), [&](const auto& l) { return l.second.rec->workshop == workshop; });
}
uint64_t version() { return S().version.load(); }

void applied_push(Applied a) {
    Store& s = S();
    std::lock_guard lock(s.mu);
    if (s.applied.size() >= kMaxApplied) s.applied.pop_front();
    s.applied.push_back(std::move(a));
}
std::string drain_applied_text(bool clear) {
    Store& s = S();
    std::lock_guard lock(s.mu);
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (const Applied& a : s.applied)
        arr.push_back({{"definition", a.definition}, {"workshop", a.workshop}, {"generation", a.generation}, {"fromServer", a.from_server},
                       {"ok", a.ok}, {"reason", a.reason}});
    if (clear) s.applied.clear();
    return arr.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

void reset_for_tests() {
    Store& s = S();
    std::lock_guard lock(s.mu);
    s.layers.clear();
    s.next_handle = 1;
    s.next_seq = 1;
    s.generation = 0;
    s.bytes = 0;
    s.has_remote = false;
    s.remote = Remote{};
    s.applied.clear();
    s.version.fetch_add(1);
    s.pending.store(false);
    s.materialized.store(0);
}
void set_generation_for_tests(uint64_t generation) {
    std::lock_guard lock(S().mu);
    S().generation = generation;
}

// ---- the C table ----
namespace {
KilnResult KILN_CALL t_info(KilnStationMenusInfo* out) { return info(out); }

KilnResult KILN_CALL t_define(KilnOwner owner, KilnStr record, KilnHandle* out, uint64_t* out_generation, char* report, size_t cap,
                              size_t* out_len) {
    std::string state = "refused", error;
    KilnResult rc;
    try {
        if (!record.ptr && record.len) {
            error = "no record";
            rc = KILN_ERR_INVALID_ARGUMENT;
        } else {
            rc = define(owner, std::string_view(record.ptr ? record.ptr : "", record.ptr ? record.len : 0), out, out_generation, &state, &error);
        }
    } catch (const std::exception& e) {
        error = std::string("kiln.stationmenus: ") + e.what();
        rc = KILN_ERR_INTERNAL;
    }
    put_report(report, cap, out_len, rc == KILN_OK, rc == KILN_OK ? state : "refused", rc == KILN_OK ? "" : error);
    return rc;
}
KilnResult KILN_CALL t_remove(KilnOwner caller, KilnHandle handle, uint64_t* out_generation) {
    try {
        return remove(caller, handle, out_generation);
    } catch (const std::exception&) {
        return KILN_ERR_INTERNAL;
    }
}
KilnResult KILN_CALL t_export(char* out, size_t cap, size_t* out_len) {
    try {
        return put_text(export_layouts(), out, cap, out_len);
    } catch (const std::exception&) {
        return KILN_ERR_INTERNAL;
    }
}
KilnResult KILN_CALL t_remote_replace(KilnOwner owner, KilnStr snapshot, char* report, size_t cap, size_t* out_len) {
    std::string state = "refused", error;
    KilnResult rc;
    try {
        if (!snapshot.ptr && snapshot.len) {
            error = "no snapshot";
            rc = KILN_ERR_INVALID_ARGUMENT;
        } else {
            rc = remote_replace(owner, std::string_view(snapshot.ptr ? snapshot.ptr : "", snapshot.ptr ? snapshot.len : 0), &state, &error);
        }
    } catch (const std::exception& e) {
        error = std::string("kiln.stationmenus: ") + e.what();
        rc = KILN_ERR_INTERNAL;
    }
    put_report(report, cap, out_len, rc == KILN_OK, rc == KILN_OK ? state : "refused", rc == KILN_OK ? "" : error);
    return rc;
}
KilnResult KILN_CALL t_remote_clear(KilnOwner owner) { return remote_clear(owner); }
KilnResult KILN_CALL t_set_pending(uint32_t on) {
    if (on > 1) return KILN_ERR_INVALID_ARGUMENT;
    set_pending(on == 1);
    return KILN_OK;
}
KilnResult KILN_CALL t_drain_applied(char* out, size_t cap, size_t* out_len) {
    try {
        // asked for the length (or too small): the queue stays as it is
        const std::string all = drain_applied_text(false);
        if (out_len) *out_len = all.size();
        if (!out || cap < all.size() + 1) return KILN_ERR_BUFFER_TOO_SMALL;
        // entries can come between the two reads (the menu hook): take them out only now, and only if they fit
        Store& s = S();
        std::lock_guard lock(s.mu);
        nlohmann::ordered_json arr = nlohmann::ordered_json::array();
        for (const Applied& a : s.applied)
            arr.push_back({{"definition", a.definition}, {"workshop", a.workshop}, {"generation", a.generation},
                           {"fromServer", a.from_server}, {"ok", a.ok}, {"reason", a.reason}});
        const std::string now = arr.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
        const KilnResult rc = put_text(now, out, cap, out_len);
        if (rc == KILN_OK) s.applied.clear();
        return rc;
    } catch (const std::exception&) {
        return KILN_ERR_INTERNAL;
    }
}
const KilnStationMenusApi kTable = {
    {sizeof(KilnStationMenusApi), KILN_STATION_MENUS_VERSION},
    &t_info, &t_define, &t_remove, &t_export, &t_remote_replace, &t_remote_clear, &t_set_pending, &t_drain_applied,
};
}  // namespace

const KilnStationMenusApi* table() { return &kTable; }

// ============================================================================================= the materializer ===
namespace {
struct Blob {
    uintptr_t data = 0;
    uint32_t count = 0;
};
// a BlobArray header as its 8 bytes say, at field
Blob at(uintptr_t field, uint64_t hdr) {
    int32_t off = 0;
    std::memcpy(&off, &hdr, 4);
    return Blob{off ? field + static_cast<intptr_t>(off) : 0, static_cast<uint32_t>(hdr >> 32)};
}
// a BlobArray header in (game) memory
Blob blob(MenuIo& io, uintptr_t field) {
    int32_t off = 0;
    uint32_t n = 0;
    if (!io.read(field, &off, 4) || !io.read(field + 4, &n, 4) || n > 100000) return {};
    Blob b{off ? field + static_cast<intptr_t>(off) : 0, n};
    if (!b.data) b.count = 0;
    return b;
}
// writes a BlobArray header (self-relative offset from field_addr, count) into field_bytes; false when out of rel32 reach
bool point(uint8_t* field_bytes, uintptr_t field_addr, uintptr_t data, uint32_t count) {
    const int64_t d = data ? static_cast<int64_t>(data) - static_cast<int64_t>(field_addr) : 0;
    if (d < INT32_MIN || d > INT32_MAX) return false;
    const int32_t o = static_cast<int32_t>(d);
    std::memcpy(field_bytes, &o, 4);
    std::memcpy(field_bytes + 4, &count, 4);
    return true;
}
// a Kiln loca tag for these texts: the GUID's identity is the text (a changed text is a new tag, so a cached older menu
// keeps its own words), the loca hooks answer FNV-1a of the GUID
void label(MenuIo& io, const std::string& key, const TextMap& texts, uint8_t* out) {
    uint8_t g[16];
    io.guid(key, g);
    io.texts(fnv32(g, 16), texts);
    std::memcpy(out, g, 16);
}
size_t layout_bytes(const Record& r, const Geometry& g) {
    size_t n = r.tabs.size() * g.gs;
    for (const MenuTab& t : r.tabs) {
        n += t.sets.size() * g.ss;
        for (const MenuSet& s : t.sets) n += s.recipes.size() * 4;
    }
    return n;
}
const char* word(TreeOutcome o) {
    switch (o) {
    case TreeOutcome::Ok: return "Ok";
    case TreeOutcome::IconSourceMissing: return "IconSourceMissing";
    case TreeOutcome::LayoutUnknown: return "LayoutUnknown";
    case TreeOutcome::NoMemory: return "NoMemory";
    case TreeOutcome::OutOfReach: return "OutOfReach";
    }
    return "?";
}
// The placements' tree (the way before layouts): the tabs and sets of the menu it takes, as many as its recipes use, each
// listing only its recipes placed there; ids of their own, the game's labels and icons.
bool placements_tree(const OwnWorkshop& m, Blob tabs, const Geometry& g, MenuIo& io, uintptr_t anchor, TreeRef* out, uint32_t* listed,
                     size_t* bytes) {
    if (!tabs.data || !tabs.count) return false;
    const uint32_t gs = g.gs, ss = g.ss;
    // the tabs and subcategories its recipes use, in the menu's order (a place past the game's tabs, a tab of Kiln's: its last)
    std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> used;
    for (const OwnPlace& p : m.places) {
        const uint32_t t = std::min(p.tab, tabs.count - 1);
        const uint32_t subs = blob(io, tabs.data + t * uintptr_t(gs) + g.cat_subs).count;
        if (!subs) continue;
        auto& list = used[t][p.tab < tabs.count ? std::min(p.sub, subs - 1) : subs - 1];
        if (std::find(list.begin(), list.end(), p.id) == list.end()) list.push_back(p.id);
    }
    if (used.empty()) return false;
    auto* groups = static_cast<uint8_t*>(io.alloc(anchor, used.size() * gs));
    if (!groups) return false;
    *bytes += used.size() * gs;
    const uintptr_t G = reinterpret_cast<uintptr_t>(groups);
    uint32_t ti = 0;
    for (const auto& [t, subs] : used) {
        const uintptr_t src = tabs.data + t * uintptr_t(gs);
        uint8_t* tab = groups + size_t(ti) * gs;
        if (!io.read(src, tab, gs)) return false;
        if (g.labels()) {
            char key[48];
            std::snprintf(key, sizeof key, "kiln.own:%x:%u", m.workshop, t);
            io.guid(key, tab + g.g_id);
        }
        const Blob os = blob(io, src + g.cat_subs);
        auto* sets = static_cast<uint8_t*>(io.alloc(anchor, subs.size() * ss));
        if (!sets) return false;
        *bytes += subs.size() * ss;
        const uintptr_t S = reinterpret_cast<uintptr_t>(sets);
        uint32_t si = 0;
        for (const auto& [s, rids] : subs) {
            uint8_t* set = sets + size_t(si) * ss;
            auto* list = static_cast<uint32_t*>(io.alloc(anchor, rids.size() * 4));
            if (!list || !io.read(os.data + s * uintptr_t(ss), set, ss)) return false;
            *bytes += rids.size() * 4;
            if (g.labels()) {
                char key[48];
                std::snprintf(key, sizeof key, "kiln.own:%x:%u:%u", m.workshop, t, s);
                io.guid(key, set + g.s_id);
            }
            std::memcpy(list, rids.data(), rids.size() * 4);
            if (!point(set + g.sub_recipes, S + si * uintptr_t(ss) + g.sub_recipes, reinterpret_cast<uintptr_t>(list),
                       static_cast<uint32_t>(rids.size())))
                return false;
            *listed += static_cast<uint32_t>(rids.size());
            ++si;
        }
        if (!point(tab + g.cat_subs, G + ti * uintptr_t(gs) + g.cat_subs, S, si)) return false;
        ++ti;
    }
    *out = TreeRef{G, ti};
    return true;
}
std::string report_key(const OwnWorkshop& m, bool ok, const std::string& reason) {
    return m.layout.rec->def_id_text + "|" + std::to_string(m.layout.generation) + "|" + (m.layout.from_server ? "s" : "l") + "|" +
           (ok ? "ok" : reason);
}
}  // namespace

TreeOutcome layout_tree(const Record& r, uintptr_t base_groups, uint32_t base_count, bool base_found, const Geometry& g, MenuIo& io,
                        uintptr_t anchor, TreeRef* out, size_t* bytes, std::string* why) {
    *out = TreeRef{};
    char b[256];
    if (!g.labels()) {
        *why = "the crafting menu's tab and set layout (labels, ids) is not known on this game build";
        return TreeOutcome::LayoutUnknown;
    }
    for (const MenuTab& t : r.tabs) {
        if (!base_found || !base_groups) {
            std::snprintf(b, sizeof b, "the menu it is like (workshop 0x%08x) is not in the game's crafting menu", r.base);
            *why = b;
            return TreeOutcome::IconSourceMissing;
        }
        if (t.icon >= base_count) {
            std::snprintf(b, sizeof b, "tab \"%s\" shows the icon of tab %u of workshop 0x%08x's menu, which has %u tab(s)", t.key.c_str(), t.icon,
                          r.base, base_count);
            *why = b;
            return TreeOutcome::IconSourceMissing;
        }
    }
    if (r.tabs.empty()) return TreeOutcome::Ok;  // a menu of no tabs: an empty tree
    const uint32_t gs = g.gs, ss = g.ss;
    auto* groups = static_cast<uint8_t*>(io.alloc(anchor, r.tabs.size() * gs));
    if (!groups) return *why = "no memory near the crafting menu", TreeOutcome::NoMemory;
    *bytes += r.tabs.size() * gs;
    const uintptr_t G = reinterpret_cast<uintptr_t>(groups);
    for (size_t i = 0; i < r.tabs.size(); ++i) {
        const MenuTab& t = r.tabs[i];
        uint8_t* tab = groups + i * gs;
        // the source tab whole (its icon), then the layout's id, label and sets
        if (!io.read(base_groups + t.icon * uintptr_t(gs), tab, gs)) {
            std::snprintf(b, sizeof b, "tab %u of workshop 0x%08x's menu could not be read", t.icon, r.base);
            *why = b;
            return TreeOutcome::IconSourceMissing;
        }
        std::memcpy(tab + g.g_id, t.id, 16);
        label(io, "kiln.stationmenu:tab:" + t.id_text + ":" + t.name_json, t.name, tab + g.g_label);
        const uintptr_t field = G + i * uintptr_t(gs) + g.cat_subs;
        if (t.sets.empty()) {
            point(tab + g.cat_subs, field, 0, 0);
            continue;
        }
        auto* sets = static_cast<uint8_t*>(io.alloc(anchor, t.sets.size() * ss));
        if (!sets) return *why = "no memory near the crafting menu", TreeOutcome::NoMemory;
        *bytes += t.sets.size() * ss;
        std::memset(sets, 0, t.sets.size() * ss);
        const uintptr_t S = reinterpret_cast<uintptr_t>(sets);
        for (size_t k = 0; k < t.sets.size(); ++k) {
            const MenuSet& s = t.sets[k];
            uint8_t* set = sets + k * ss;
            if (s.has_heading) label(io, "kiln.stationmenu:set:" + s.id_text + ":" + s.heading_json, s.heading, set + g.s_label);  // else none
            std::memcpy(set + g.s_id, s.id, 16);
            const uintptr_t entries = S + k * uintptr_t(ss) + g.sub_recipes;
            if (s.recipes.empty()) {
                point(set + g.sub_recipes, entries, 0, 0);
                continue;
            }
            auto* ids = static_cast<uint32_t*>(io.alloc(anchor, s.recipes.size() * 4));
            if (!ids) return *why = "no memory near the crafting menu", TreeOutcome::NoMemory;
            *bytes += s.recipes.size() * 4;
            std::memcpy(ids, s.recipes.data(), s.recipes.size() * 4);
            if (!point(set + g.sub_recipes, entries, reinterpret_cast<uintptr_t>(ids), static_cast<uint32_t>(s.recipes.size())))
                return *why = "a list out of reach of the crafting menu", TreeOutcome::OutOfReach;
        }
        if (!point(tab + g.cat_subs, field, S, static_cast<uint32_t>(t.sets.size())))
            return *why = "a list out of reach of the crafting menu", TreeOutcome::OutOfReach;
    }
    *out = TreeRef{G, static_cast<uint32_t>(r.tabs.size())};
    return TreeOutcome::Ok;
}

OwnResult own_publish(uintptr_t coll, const std::vector<OwnWorkshop>& menus, const Geometry& g, MenuIo& io, OwnState& st) {
    OwnResult res;
    // per layout workshop: its tree, or why not; reported once per change when the build ends
    struct Status {
        const OwnWorkshop* m;
        bool built = false;
        TreeRef tree;
        std::string reason;
    };
    std::vector<Status> status;
    for (const OwnWorkshop& m : menus)
        if (m.layout.rec) status.push_back(Status{&m, false, TreeRef{}, std::string()});
    const auto finish = [&](bool published, const char* fault) {
        for (Status& s : status) {
            const bool ok = published && s.built;
            if (ok) st.good[s.m->workshop] = s.tree;
            std::string reason = ok ? "" : !s.reason.empty() ? s.reason : std::string("PublishFailed: ") + fault;
            const std::string key = report_key(*s.m, ok, reason);
            if (st.said[s.m->workshop] == key) continue;
            st.said[s.m->workshop] = key;
            char b[160];
            std::snprintf(b, sizeof b, "station menu of workshop 0x%08x (%s %s, generation %llu)", s.m->workshop,
                          s.m->layout.from_server ? "the server's" : "layout", s.m->layout.rec->def_key.c_str(),
                          static_cast<unsigned long long>(s.m->layout.generation));
            res.notes.emplace_back(!ok, std::string(b) + (ok ? ": shown" : ": not shown, " + reason));
            applied_push(Applied{s.m->layout.rec->def_id_text, s.m->workshop, s.m->layout.generation, s.m->layout.from_server, ok, reason});
        }
    };
    const auto write1 = [&](int i, const uint64_t& v) {
        std::vector<Write> w{Write{coll + 8ull * i, 8, {}}};
        std::memcpy(w[0].bytes, &v, 8);
        return io.write(w, nullptr) == 1;
    };
    // After a header write that did not land: both back to the game's, each on its own (one header alone is harmless, a
    // workshop past the end of the trees has none, but both go back). A header that cannot be written at all keeps what it
    // had; then the other goes back to its partner of the pair shown before, so that both always come from one build.
    const auto settle = [&](bool was_on) {
        write1(0, st.orig[0]);
        write1(1, st.orig[1]);
        uint64_t now[2] = {};
        const bool seen = io.read(coll, &now[0], 8) && io.read(coll + 8, &now[1], 8);
        if (!seen || (now[0] == st.orig[0] && now[1] == st.orig[1]) || !was_on) {
            st.on = false;
            return;
        }
        for (int i = 0; i < 2; ++i)
            if (now[i] != st.shown[i]) write1(i, st.shown[i]);
        st.on = io.read(coll, &now[0], 8) && io.read(coll + 8, &now[1], 8) && now[0] == st.shown[0] && now[1] == st.shown[1];
    };
    const auto none_left = [&]() {
        if (st.on) {
            std::vector<Write> writes;
            for (int i = 0; i < 2; ++i) {
                Write w{coll + 8ull * i, 8, {}};
                std::memcpy(w.bytes, &st.orig[i], 8);
                writes.push_back(w);
            }
            uintptr_t failed = 0;
            if (io.write(writes, &failed) == 2) st.on = false;
            else settle(true);
            res.restored = !st.on;
        }
        res.ok = !st.on;
    };
    if (menus.empty()) {
        none_left();
        return res;
    }
    // the game's arrays, as its data has them
    const Blob wss = at(coll, st.orig[0]), trs = at(coll + 8, st.orig[1]);
    const uint32_t n = wss.count, k = static_cast<uint32_t>(menus.size()), gs = g.gs;
    if (!wss.data || !trs.data || trs.count < n || st.trees.size() < n) {
        finish(false, "the crafting menu's lists could not be read");
        return res;
    }
    const size_t lists = size_t(n + k) * 12;
    if (st.backing + lists > kMaxBacking) {
        for (Status& s : status)
            if (s.reason.empty()) s.reason = "RestartRequired: 64 MiB of crafting-menu memory are in use; the menu changes after a restart";
        finish(false, "");
        return res;
    }
    auto* ids = static_cast<uint32_t*>(io.alloc(coll, size_t(n + k) * 4));
    auto* trees = ids ? static_cast<uint8_t*>(io.alloc(coll, size_t(n + k) * 8)) : nullptr;
    if (ids) st.backing += size_t(n + k) * 4;
    if (trees) st.backing += size_t(n + k) * 8;
    if (!ids || !trees || !io.read(wss.data, ids, size_t(n) * 4)) {
        finish(false, "no memory near the crafting menu");
        return res;
    }
    const uintptr_t T = reinterpret_cast<uintptr_t>(trees);
    for (uint32_t i = 0; i < n; ++i) {
        const Blob t = at(trs.data + i * 8ull, st.trees[i]);
        if (!point(trees + i * 8ull, T + i * 8ull, t.data, t.count)) {
            finish(false, "a list out of reach of the crafting menu");
            return res;
        }
    }
    const auto index_of = [&](uint32_t w) {
        for (uint32_t i = 0; i < n; ++i)
            if (ids[i] == w) return i;
        return UINT32_MAX;
    };
    uint32_t j = n;
    size_t si = 0;
    for (const OwnWorkshop& m : menus) {
        TreeRef tree;
        bool have = false;
        std::string what;
        if (m.layout.rec) {
            Status& s = status[si++];
            const Record& r = *m.layout.rec;
            const auto key = std::make_pair(m.workshop, r.hash);
            if (const auto c = st.cache.find(key); c != st.cache.end()) {
                s.tree = c->second;
                s.built = true;
            } else if (st.built[m.workshop] >= kMaxBuiltPerWorkshop) {
                char b[192];
                std::snprintf(b, sizeof b, "RestartRequired: workshop 0x%08x had %u menus built in this game; the next shows after a restart",
                              m.workshop, kMaxBuiltPerWorkshop);
                s.reason = b;
            } else if (st.backing + layout_bytes(r, g) > kMaxBacking) {
                s.reason = "RestartRequired: 64 MiB of crafting-menu memory are in use; the menu changes after a restart";
            } else {
                const uint32_t bi = index_of(r.base);
                const Blob base = bi != UINT32_MAX ? at(trs.data + bi * 8ull, st.trees[bi]) : Blob{};
                size_t used = 0;
                std::string why;
                const TreeOutcome o = layout_tree(r, base.data, base.count, bi != UINT32_MAX, g, io, coll, &s.tree, &used, &why);
                st.backing += used;
                if (o == TreeOutcome::NoMemory || o == TreeOutcome::OutOfReach) {
                    finish(false, why.c_str());  // as with the placements: nothing is published without memory near the menu
                    return res;
                }
                if (o == TreeOutcome::Ok) {
                    st.cache[key] = s.tree;
                    ++st.built[m.workshop];
                    S().materialized.fetch_add(1);
                    s.built = true;
                } else {
                    s.reason = std::string(word(o)) + ": " + why;
                }
            }
            if (s.built) {
                tree = s.tree;
                have = true;
                char b[128];
                std::snprintf(b, sizeof b, "0x%08x (layout %s, %u tab(s))", m.workshop, r.def_key.c_str(), tree.count);
                what = b;
            } else if (const auto gd = st.good.find(m.workshop); gd != st.good.end()) {
                tree = gd->second;  // the last menu of this workshop that was shown stays
                have = true;
                char b[96];
                std::snprintf(b, sizeof b, "0x%08x (its last layout kept)", m.workshop);
                what = b;
            }
        }
        if (!have && m.menu && !m.places.empty()) {
            const uint32_t mi = index_of(m.menu);
            if (mi == UINT32_MAX) {
                finish(false, "the menu a station takes its tabs from is not in the crafting menu");
                return res;
            }
            uint32_t listed = 0;
            size_t used = 0;
            const bool ok = placements_tree(m, at(trs.data + mi * 8ull, st.trees[mi]), g, io, coll, &tree, &listed, &used);
            st.backing += used;
            if (!ok) {
                finish(false, "a station's own menu could not be made");
                return res;
            }
            have = true;
            char b[96];
            std::snprintf(b, sizeof b, "0x%08x (%u recipe(s) in %u tab(s) of 0x%08x's)", m.workshop, listed, tree.count, m.menu);
            what = b;
        }
        if (!have) continue;  // a refused layout with nothing to show: the workshop has no menu of its own
        ids[j] = m.workshop;
        if (!point(trees + j * 8ull, T + j * 8ull, tree.groups, tree.count)) {
            finish(false, "a list out of reach of the crafting menu");
            return res;
        }
        ++j;
        res.said += (res.said.empty() ? "" : ", ") + what;
    }
    (void)gs;
    if (j == n) {  // nothing of Kiln's to list: the game's own lists
        none_left();
        finish(false, "");
        return res;
    }
    Write wi{}, wt{};
    wi.addr = coll, wi.n = 8;
    wt.addr = coll + 8, wt.n = 8;
    if (!point(wi.bytes, coll, reinterpret_cast<uintptr_t>(ids), j) || !point(wt.bytes, coll + 8, T, j)) {
        finish(false, "a list out of reach of the crafting menu");
        return res;
    }
    std::vector<Write> writes{wi, wt};
    if (io.write(writes, &res.failed) != 2) {
        settle(st.on);
        finish(false, "the crafting menu's lists could not be written");
        return res;
    }
    st.on = true;
    std::memcpy(&st.shown[0], wi.bytes, 8);
    std::memcpy(&st.shown[1], wt.bytes, 8);
    res.ok = res.published = true;
    res.own = j - n;
    finish(true, "");
    return res;
}

uint32_t absent_id(const std::vector<uint32_t>& live) {
    std::vector<uint32_t> v(live);
    std::sort(v.begin(), v.end());
    uint32_t id = 1;
    for (uint32_t x : v) {
        if (x == id) {
            if (++id == 0) return 0;
        } else if (x > id) {
            break;
        }
    }
    return id;
}

uint32_t hold_workshop(uint32_t workshop, const std::vector<uint32_t>& game, const std::vector<uint32_t>& live, bool def_own) {
    if (!workshop || !pending()) return 0;
    const bool own = def_own || has_layout(workshop) || (!game.empty() && std::find(game.begin(), game.end(), workshop) == game.end());
    return own ? absent_id(live) : 0;
}

}  // namespace kc::ml
