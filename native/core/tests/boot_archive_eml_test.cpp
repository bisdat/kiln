// SPDX-License-Identifier: MIT
// Host tests of kiln.boot in a kiln-backend start (src/services/boot_archive_eml.h, boot_service.cpp's kiln-backend
// base), against a fake kiln.eml.archive C table (the table's real layout, kiln_eml_archive.h):
//  - the view's binding: every refusal of the table, the identity source and the identity's pins, acquire and the
//    view's own identity, each leaving OnBoot without game data and with its reason;
//  - the reads: kiln.boot's query forms to resource_json's, the reply's pin, the translation of the lossless tree into
//    the snapshot's value form, every provider refusal (not found, unavailable with its cause, revoked, fault, a reply
//    that changes size), the result and budget limits, the bytes through read_resource;
//  - the lifetime: release waits for the reads in flight and no read reaches the provider after it (also from 8
//    threads, the TSan build);
//  - kiln.boot over it: an old snapshot of the same game build with other values is never read in a kiln-backend start
//    (the author's query answers the committed value; a hosted start answers the snapshot's), no fallback when the
//    generation is missing, and the optional / required OnBoot policy through the fake exit adapter;
//  - the production identity source (published_identity_of) over a fake kiln.eml.startup table: the session-free
//    key, every refusal (no table, a table without the member, no grant, a refused key, a fault, a short record) and
//    kiln.boot end to end over it.
// PRIVATE (tools/sfi/public_cut.py). Built with ASan/UBSan and with TSan by test-boot-archive-eml.sh.
// Prints one line per case and "BOOT ARCHIVE EML TESTS <n> PASS <m> FAIL"; exits 1 when anything failed.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>  // tests/stubs/windows.h
struct EXCEPTION_POINTERS;
#ifndef _WIN32
#define __try try
#define __except(x) catch (...)
#endif
[[noreturn]] inline void ExitProcess(unsigned int) { std::abort(); }  // the adapter is replaced before any call
inline void* GetCurrentProcess() { return nullptr; }
inline int TerminateProcess(void*, unsigned int) { std::abort(); }  // the late adapter is replaced too

#include "../src/services/service.cpp"
#include "../src/services/boot_service.cpp"


namespace fs = std::filesystem;
using nlohmann::json;

// ---- stubs ----
namespace {
std::vector<std::string> t_log;
std::map<KilnOwner, std::string> t_owners;
std::vector<KilnOwner> t_revoked;
KilnOwner t_next_owner = 100;
kiln::Config t_config;
kiln::ImageInfo t_image;
kiln::Paths t_paths;
uintptr_t t_worlds[KILN_WORLD_KINDS]{};
uint32_t t_sessions[KILN_WORLD_KINDS]{};  // tick::session's worlds per kind since boot
kiln::profile::State t_profile;
uint32_t t_dropins = 1u << KILN_PROFILE_KIND_KILN;

// The capability registry, reduced: per name the reports of each owner, the worst shown (kiln_caps.h).
std::map<std::string, std::map<KilnOwner, KilnCapRow>> t_caps;
std::map<std::string, std::string> t_texts;  // storage for the shown row's text
KilnResult KILN_CALL fake_report(KilnOwner owner, KilnStr name, uint32_t state, uint32_t reason, KilnStr text) {
    static std::map<std::string, std::string> keep;
    const std::string n(name.ptr, name.len), key = n + "#" + std::to_string(owner);
    keep[key] = std::string(text.ptr ? text.ptr : "", text.len);
    KilnCapRow row{};
    row.size = sizeof row;
    row.state = state;
    row.reason = reason;
    row.text = KilnStr{keep[key].data(), keep[key].size()};
    t_caps[n][owner] = row;
    return KILN_OK;
}
KilnResult KILN_CALL fake_find(KilnStr name, KilnCapRow* out) {
    const auto it = t_caps.find(std::string(name.ptr, name.len));
    if (it == t_caps.end() || it->second.empty()) return KILN_ERR_NOT_FOUND;
    const KilnCapRow* w = nullptr;
    auto rank = [](uint32_t s) { return s == KILN_CAP_UNAVAILABLE ? 0 : s == KILN_CAP_PENDING ? 1 : 2; };
    for (const auto& [o, r] : it->second)
        if (!w || rank(r.state) < rank(w->state)) w = &r;
    *out = *w;
    return KILN_OK;
}
KilnCapsApi t_caps_table = [] {
    KilnCapsApi t{};
    t.header = {sizeof(KilnCapsApi), 1};
    t.report = &fake_report;
    t.find = &fake_find;
    return t;
}();
const KilnCapRow* shown(const std::string& name) {
    static KilnCapRow r;
    r.size = sizeof r;
    return fake_find(KilnStr{name.data(), name.size()}, &r) == KILN_OK ? &r : nullptr;
}
}  // namespace

namespace kiln {
const Config& config() { return t_config; }
const ImageInfo& image() { return t_image; }
const Paths& paths() { return t_paths; }
double now_ms() { return 0; }
std::string to_utf8(std::wstring_view w) { return std::string(w.begin(), w.end()); }
std::wstring to_wide(std::string_view s) { return std::wstring(s.begin(), s.end()); }
bool read_file(const std::filesystem::path& file, std::string* out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), {});
    return true;
}
std::string jsonc_to_json(std::string_view text) { return std::string(text); }  // the fixtures are plain JSON
namespace log {
std::mutex t_log_mutex;  // the threaded cases log from several threads
void writef(int, const char*, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::lock_guard lk(t_log_mutex);
    t_log.emplace_back(b);
}
}  // namespace log
namespace reg {
KilnOwner owner_create(std::string_view id) {
    t_owners[t_next_owner] = std::string(id);
    return t_next_owner++;
}
void owner_revoke(KilnOwner owner) {
    t_owners.erase(owner);
    t_revoked.push_back(owner);
}
std::string owner_name(KilnOwner owner) {
    const auto it = t_owners.find(owner);
    return it == t_owners.end() ? "(revoked)" : it->second;
}
}  // namespace reg
namespace worldinfo {
uintptr_t world(uint32_t kind) { return kind < KILN_WORLD_KINDS ? t_worlds[kind] : 0; }
}
namespace tick {
KilnResult session(KilnSessionInfo* out) {
    for (uint32_t k = 0; k < KILN_WORLD_KINDS; ++k) out->sessions[k] = t_sessions[k];
    return KILN_OK;
}
}
namespace profile {
const State& state() { return t_profile; }
bool dropins(uint32_t kind) { return (t_dropins & (1u << kind)) != 0; }
}  // namespace profile
namespace crash {
int filter(EXCEPTION_POINTERS*, Capture*, unsigned long*) { return 1; }
void report(const Capture&, const char*, const void*) {}
}  // namespace crash
namespace caps {
const KilnCapsApi* table() { return &t_caps_table; }
}  // namespace caps
}  // namespace kiln


// ---- the checks ----
namespace {
using kiln::boot_view::CommittedView;
using kiln::boot_view::LaunchFacts;
using ojson = nlohmann::ordered_json;
namespace boot_gd = kiln::boot_gd;

int t_pass = 0, t_fail = 0;
std::string t_case;
#define CHECK(x) check((x), #x, __LINE__)
void check(bool ok, const char* what, int line) {
    if (ok) {
        ++t_pass;
        return;
    }
    ++t_fail;
    std::printf("FAIL %s: %s (line %d)\n", t_case.c_str(), what, line);
}
void pass_line(const char* name, int fails_before) { std::printf("%s %s\n", t_fail == fails_before ? "PASS" : "FAIL", name); }

// ---- tagged values (common.schema.json's value) ----
std::string hexs(const std::string& b) { return kiln::boot_view::hex_bytes(b.data(), b.size()); }
json tv_u64(uint64_t v, const char* type = "uint32") {
    char s[24];
    std::snprintf(s, sizeof s, "u64:%016llx", static_cast<unsigned long long>(v));
    return {{"tag", "u64"}, {"type", type}, {"value", s}};
}
json tv_i64(int64_t v, const char* type = "sint32") {
    char s[24];
    std::snprintf(s, sizeof s, "i64:%016llx", static_cast<unsigned long long>(v));
    return {{"tag", "i64"}, {"type", type}, {"value", s}};
}
json tv_f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    char s[16];
    std::snprintf(s, sizeof s, "f32:%08x", u);
    return {{"tag", "f32"}, {"type", "float32"}, {"value", s}};
}
json tv_f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    char s[24];
    std::snprintf(s, sizeof s, "f64:%016llx", static_cast<unsigned long long>(u));
    return {{"tag", "f64"}, {"type", "float64"}, {"value", s}};
}
json tv_str(const std::string& s) { return {{"tag", "string"}, {"value", "string:" + hexs(s)}}; }
json tv_bytes(const std::string& s) { return {{"tag", "bytes"}, {"value", "bytes:" + hexs(s)}}; }
json tv_guid(const std::string& game_order_hex, const char* tag = "guid") { return {{"tag", tag}, {"value", "guid:" + game_order_hex}}; }
json tv_map(const std::vector<std::pair<std::string, json>>& fields, const char* type = "keen::TestStruct") {
    json entries = json::array();
    for (const auto& [k, v] : fields) entries.push_back({{"key", tv_str(k)}, {"value", v}});
    return {{"tag", "map"}, {"type", type}, {"entries", entries}};
}
json tv_array(const std::vector<json>& items) { return {{"tag", "array"}, {"type", "keen::BlobArray<keen::TestStruct>"}, {"items", items}}; }

// The snapshot's item and the committed generation's same item with another stack size.
const std::string kGuidItem = "0f8fad5b-d9cb-469f-a165-70867728950e";
const std::string kGuidItemGame = "5bad8f0fcbd99f46a16570867728950e";  // the same GUID's bytes in the game's order
json item_value(uint64_t stack) {
    return tv_map({{"name", tv_str("Test_Item_Logs")}, {"stackSize", tv_u64(stack)}, {"tags", tv_array({tv_str("wood"), tv_str("fuel")})}},
                  "keen::ItemInfo");
}

// ---- a fake kiln.eml.archive (the provider side of kiln_eml_archive.h, its record rules reduced to what kiln.boot uses) ----
struct FakeArchive {
    std::mutex m;
    KilnEmlArchiveIdentityV1 granted{};  // the generation acquire accepts (all pins equal)
    KilnEmlArchiveIdentityV1 shown{};    // what identity() answers for a view (normally granted)
    KilnResult acquire_rc = KILN_OK;
    std::map<std::string, std::string> replies;                        // query -> resource_json reply
    std::map<std::string, std::pair<KilnResult, std::string>> fails;  // query -> result and the cause behind its token
    std::map<std::string, std::string> res;                            // key bytes -> read_resource bytes
    std::map<std::string, int> grow;                                   // query -> copies that find it larger than its probe said
    uint64_t next_view = 0x5000, next_token = 0x9000;
    std::set<uint64_t> live, released;
    std::map<uint64_t, std::string> tokens;  // live error tokens -> their message
    int tokens_released = 0, acquires = 0, releases = 0;
    std::atomic<int> calls{0}, late{0};  // reads; reads on a view after its release
    std::atomic<int> hold_ms{0};         // a read sleeps this long inside (the release-waits-for-reads case)
    std::vector<std::string> queries;
};
FakeArchive* F = nullptr;

bool same_pins(const KilnEmlArchiveIdentityV1& a, const KilnEmlArchiveIdentityV1& b) {
    KilnEmlArchiveIdentityV1 x = a, y = b;
    x.size = y.size = 0;
    return std::memcmp(&x, &y, sizeof x) == 0;
}
void fill_error(KilnArchiveErrorV1* e, KilnResult rc, const std::string& cause) {
    if (!e) return;
    e->result = rc;
    e->token = 0;
    e->message_bytes = 0;
    if (cause.empty()) return;
    e->token = F->next_token++;
    e->message_bytes = cause.size();
    F->tokens[e->token] = cause;
}
bool error_ok(KilnArchiveErrorV1* e) {
    if (!e) return true;
    if (e->size < sizeof(KilnArchiveErrorV1) || e->flags || e->reserved) return false;
    e->result = KILN_OK, e->token = 0, e->message_bytes = 0;
    return true;
}

KilnResult KILN_CALL fa_acquire(KilnOwner owner, const KilnEmlArchiveIdentityV1* expected, KilnEmlArchiveView* out) {
    std::lock_guard lk(F->m);
    ++F->acquires;
    if (!owner || !expected || !out) return KILN_ERR_INVALID_ARGUMENT;
    if (F->acquire_rc != KILN_OK) return F->acquire_rc;
    if (!same_pins(*expected, F->granted)) return KILN_ERR_REVOKED;
    *out = F->next_view++;
    F->live.insert(*out);
    return KILN_OK;
}
KilnResult KILN_CALL fa_identity(KilnEmlArchiveView view, KilnEmlArchiveIdentityV1* out) {
    std::lock_guard lk(F->m);
    if (!F->live.count(view)) return KILN_ERR_REVOKED;
    const uint32_t size = out->size;
    *out = F->shown;
    out->size = size;
    return KILN_OK;
}
KilnResult KILN_CALL fa_release(KilnEmlArchiveView view) {
    std::lock_guard lk(F->m);
    ++F->releases;
    if (F->live.erase(view)) F->released.insert(view);
    return F->released.count(view) ? KILN_OK : KILN_ERR_INVALID_ARGUMENT;
}
KilnResult KILN_CALL fa_resource_json(KilnEmlArchiveView view, KilnStr query, char* json_out, size_t capacity, size_t* required,
                                      KilnArchiveErrorV1* error) {
    ++F->calls;
    if (const int ms = F->hold_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    std::lock_guard lk(F->m);
    if (F->released.count(view)) ++F->late;
    if (!error_ok(error)) return KILN_ERR_INVALID_ARGUMENT;
    if (!required || (!json_out && capacity)) return KILN_ERR_INVALID_ARGUMENT;
    if (!F->live.count(view)) return F->released.count(view) ? KILN_ERR_REVOKED : KILN_ERR_INVALID_ARGUMENT;
    const std::string q(query.ptr, query.len);
    F->queries.push_back(q);
    if (const auto f = F->fails.find(q); f != F->fails.end()) {
        fill_error(error, f->second.first, f->second.second);
        return f->second.first;
    }
    const auto r = F->replies.find(q);
    if (r == F->replies.end()) return KILN_ERR_NOT_FOUND;
    size_t size = r->second.size();
    if (auto g = F->grow.find(q); g != F->grow.end() && g->second > 0 && capacity) --g->second, size += 4;
    *required = size;
    if (capacity < size) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(json_out, r->second.data(), r->second.size());
    return KILN_OK;
}
KilnResult KILN_CALL fa_read_resource(KilnOwner owner, KilnEmlArchiveView view, const KilnAssetKey* key, void* out, size_t capacity,
                                      size_t* required, KilnArchiveErrorV1* error) {
    ++F->calls;
    std::lock_guard lk(F->m);
    if (F->released.count(view)) ++F->late;
    if (!error_ok(error) || !owner || !key || !required) return KILN_ERR_INVALID_ARGUMENT;
    if (!F->live.count(view)) return KILN_ERR_REVOKED;
    const auto it = F->res.find(std::string(reinterpret_cast<const char*>(key), sizeof *key));
    if (it == F->res.end()) return KILN_ERR_NOT_FOUND;
    *required = it->second.size();
    if (capacity < it->second.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    if (!it->second.empty()) std::memcpy(out, it->second.data(), it->second.size());
    return KILN_OK;
}
KilnResult KILN_CALL fa_error_copy(KilnOwner, KilnHandle token, void* out, size_t capacity, size_t* required) {
    std::lock_guard lk(F->m);
    const auto it = F->tokens.find(token);
    if (it == F->tokens.end()) return KILN_ERR_INVALID_ARGUMENT;
    *required = it->second.size();
    if (capacity < it->second.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, it->second.data(), it->second.size());
    return KILN_OK;
}
KilnResult KILN_CALL fa_error_release(KilnOwner, KilnHandle token) {
    std::lock_guard lk(F->m);
    if (!F->tokens.erase(token)) return KILN_ERR_INVALID_ARGUMENT;
    ++F->tokens_released;
    return KILN_OK;
}

KilnEmlArchiveApiV1 t_api{};
const KilnEmlArchiveApiV1* full_table() {
    t_api = KilnEmlArchiveApiV1{};
    t_api.header = {sizeof(KilnEmlArchiveApiV1), KILN_EML_ARCHIVE_VERSION};
    t_api.acquire = &fa_acquire;
    t_api.identity = &fa_identity;
    t_api.resource_json = &fa_resource_json;
    t_api.release = &fa_release;
    t_api.read_resource = &fa_read_resource;
    t_api.error_copy = &fa_error_copy;
    t_api.error_release = &fa_error_release;
    return &t_api;
}

// This launch and the identity kiln-eml grants for it.
constexpr uint64_t kLaunch = 0x0123456789abcdefull;
LaunchFacts launch_facts() {
    LaunchFacts l;
    l.launch_id = kLaunch;
    for (int i = 0; i < 32; ++i) l.plan_sha256[static_cast<size_t>(i)] = static_cast<uint8_t>(0xa0 + i);
    return l;
}
KilnEmlArchiveIdentityV1 identity_for(const LaunchFacts& l) {
    KilnEmlArchiveIdentityV1 id{};
    id.size = sizeof id;
    id.phase = KILN_EML_ARCHIVE_PUBLISHED;
    for (int i = 0; i < 8; ++i) id.launch_id[8 + i] = static_cast<uint8_t>(l.launch_id >> (56 - 8 * i));
    for (int i = 0; i < 16; ++i) id.plan_id[i] = static_cast<uint8_t>(0x40 + i);
    id.generation = 7;
    id.reload_epoch = 1;
    std::memcpy(id.plan_sha256, l.plan_sha256.data(), 32);
    for (int i = 0; i < 32; ++i) id.archive_sha256[i] = static_cast<uint8_t>(0x10 + i), id.registry_sha256[i] = static_cast<uint8_t>(0x70 + i);
    id.registry_id = 3;
    return id;
}
std::string reply_for(const KilnEmlArchiveIdentityV1& id, const std::string& guid_game_hex, uint32_t type, uint32_t part, const json& value) {
    return json{{"schema_version", 1},          {"pin", kiln::boot_view::pin_of(id)}, {"resourceId", "guid:" + guid_game_hex},
                {"typeHash", type},             {"part", part},                       {"type_index", "u64:0000000000000007"},
                {"value", value}}
        .dump();
}

// The identity source: what kiln-eml hands (the test sets it).
KilnEmlArchiveIdentityV1 t_handed{};
KilnResult t_handed_rc = KILN_OK;
std::string t_handed_why;
int t_identity_calls = 0;
KilnResult KILN_CALL handed(const LaunchFacts&, KilnEmlArchiveIdentityV1* out, std::string* why) {
    ++t_identity_calls;
    if (t_handed_rc != KILN_OK) return *why = t_handed_why, t_handed_rc;
    const uint32_t size = out->size;
    *out = t_handed;
    if (t_handed.size == sizeof t_handed) out->size = size;  // a source that writes a short record keeps it short
    return KILN_OK;
}

// An identity source that waits until the test lets it go on (a release while bind() is under way).
std::atomic<bool> t_source_waiting{false}, t_source_hold{false};
KilnResult blocking_handed(const LaunchFacts& l, KilnEmlArchiveIdentityV1* out, std::string* why) {
    t_source_waiting = true;
    while (t_source_hold.load()) std::this_thread::yield();
    return handed(l, out, why);
}

// A fresh fake with the granted generation and its item reply (the stack size given).
void fresh(FakeArchive& f, uint64_t stack = 75) {
    F = &f;
    const auto l = launch_facts();
    f.granted = f.shown = t_handed = identity_for(l);
    t_handed_rc = KILN_OK;
    t_handed_why.clear();
    f.replies["item:Test_Item_Logs"] = reply_for(f.granted, kGuidItemGame, 0x1a2b3c4du, 0, item_value(stack));
}

struct Read {
    KilnResult rc;
    std::string json, why;
    size_t required = 777, value = 0;
};
Read tree_of(CommittedView& v, const std::string& q, size_t capacity = 1 << 20, size_t result_max = 4u << 20, uint64_t budget = 16u << 20) {
    Read r{};
    r.rc = v.tree(q, capacity, result_max, budget, &r.json, &r.required, &r.value, &r.why);
    return r;
}

// ---- the binding ----
void bind_case() {
    t_case = "bind";
    const int f0 = t_fail;
    const auto l = launch_facts();
    std::string why;
    // each refusal leaves the view unbound with its reason, and no view held
    struct Refusal {
        const char* name;
        std::function<void(FakeArchive&, KilnEmlArchiveApiV1&, KilnEmlArchiveIdentityV1&, LaunchFacts&, kiln::boot_view::IdentitySource&)> set;
        const char* reason;
    };
    using Api = KilnEmlArchiveApiV1;
    using Id = KilnEmlArchiveIdentityV1;
    using Src = kiln::boot_view::IdentitySource;
    const std::vector<Refusal> refusals = {
        {"null table", [](FakeArchive&, Api& api, Id&, LaunchFacts&, Src&) { api.header.size = 0; }, "offers no kiln.eml.archive table"},
        {"short table", [](FakeArchive&, Api& api, Id&, LaunchFacts&, Src&) { api.header.size = offsetof(KilnEmlArchiveApiV1, release); }, "too short"},
        {"version 0", [](FakeArchive&, Api& api, Id&, LaunchFacts&, Src&) { api.header.version = 0; }, "too short"},
        {"no resource_json", [](FakeArchive&, Api& api, Id&, LaunchFacts&, Src&) { api.resource_json = nullptr; }, "too short"},
        {"no identity source", [](FakeArchive&, Api&, Id&, LaunchFacts&, Src& src) { src = nullptr; }, "no committed archive generation was handed"},
        {"no launch id", [](FakeArchive&, Api&, Id&, LaunchFacts& lf, Src&) { lf.launch_id = 0; }, "no launch id"},
        {"source refuses", [](FakeArchive&, Api&, Id&, LaunchFacts&, Src&) { t_handed_rc = KILN_ERR_UNAVAILABLE, t_handed_why = "the barrier granted nothing"; },
         "the barrier granted nothing"},
        {"prepared", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { id.phase = KILN_EML_ARCHIVE_PREPARED; }, "not the published one"},
        {"short identity", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { id.size = 152; }, "record is short"},
        {"wrong launch", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { id.launch_id[15] ^= 1; }, "belongs to another launch"},
        {"launch not big-endian", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { std::reverse(id.launch_id + 8, id.launch_id + 16); },
         "belongs to another launch"},
        {"wrong plan", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { id.plan_sha256[0] ^= 1; }, "another launch plan"},
        {"no plan id", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { std::memset(id.plan_id, 0, 16); }, "names no launch plan"},
        {"generation 0", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { id.generation = 0; }, "number or epoch"},
        {"epoch 2", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { id.reload_epoch = 2; }, "number or epoch"},
        {"no archive digest", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { std::memset(id.archive_sha256, 0, 32); }, "digest is missing"},
        {"no registry id", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { id.registry_id = 0; }, "digest is missing"},
        {"no registry digest", [](FakeArchive&, Api&, Id& id, LaunchFacts&, Src&) { std::memset(id.registry_sha256, 0, 32); }, "digest is missing"},
        {"acquire refused", [](FakeArchive& f, Api&, Id&, LaunchFacts&, Src&) { f.acquire_rc = KILN_ERR_REVOKED; }, "refused the committed archive generation's view"},
        {"another digest than granted", [](FakeArchive& f, Api&, Id&, LaunchFacts&, Src&) { f.granted.archive_sha256[3] ^= 1; },
         "refused the committed archive generation's view"},
        {"view names another", [](FakeArchive& f, Api&, Id&, LaunchFacts&, Src&) { f.shown.generation = 8; }, "names another archive generation"},
    };
    for (const auto& r : refusals) {
        FakeArchive f;
        fresh(f);
        KilnEmlArchiveApiV1 api = *full_table();
        LaunchFacts lf = l;
        Src src = &handed;
        r.set(f, api, t_handed, lf, src);
        CommittedView v;
        why.clear();
        const bool ok = v.bind(100, lf, api.header.size ? &api : nullptr, src, &why);
        t_case = std::string("bind-") + r.name;
        CHECK(!ok && why.find(r.reason) != std::string::npos);
        std::string again;
        CHECK(!v.ready(&again) && again == why && v.digest().empty());
        CHECK(f.live.empty());  // nothing held: a view acquired for another generation was given back
        CHECK(tree_of(v, "item:Test_Item_Logs").rc == KILN_ERR_UNAVAILABLE && f.calls == 0);
        // once refused, refused for the generation: bind() does not try again
        const int acquires = f.acquires;
        CHECK(!v.bind(100, l, full_table(), &handed, &why) && f.acquires == acquires);
    }
    t_case = "bind";
    t_handed_rc = KILN_OK;
    {
        FakeArchive f;
        fresh(f);
        CommittedView v;
        CHECK(!v.bind(0, l, full_table(), &handed, &why) && why.find("no owner") != std::string::npos);
    }
    // bound: one acquire with exactly the handed pins, the digest names them
    FakeArchive f;
    fresh(f);
    CommittedView v;
    CHECK(!v.ready(&why) && why.find("startup pass has run") != std::string::npos);
    CHECK(v.bind(100, l, full_table(), &handed, &why) && why.empty() && v.ready(&why) && f.live.size() == 1 && f.acquires == 1);
    CHECK(v.bind(100, l, full_table(), &handed, &why) && f.acquires == 1);  // bound once
    boot_gd::Sha256 h;
    KilnEmlArchiveIdentityV1 id = f.granted;
    h.update(kiln::boot_view::kDigestHead).update("committed\n").update(&id, sizeof id);
    CHECK(v.digest() == boot_gd::hex_of(h.final()) && std::string(v.source()) == "committed");
    {  // another generation (another archive digest): another digest
        FakeArchive g;
        fresh(g);
        g.granted.archive_sha256[0] ^= 0x80;
        g.shown = t_handed = g.granted;
        CommittedView w;
        CHECK(w.bind(100, l, full_table(), &handed, &why) && boot_gd::lower_hex(w.digest(), 64) && w.digest() != v.digest());
        w.release();
    }
    F = &f;
    v.release();
    CHECK(f.live.empty() && f.released.size() == 1);
    pass_line("EML-VIEW-BIND every table, source and pin refusal leaves OnBoot without game data, with its reason", f0);
}

// ---- the reads ----
void reads_case() {
    t_case = "reads";
    const int f0 = t_fail;
    FakeArchive f;
    fresh(f);
    const auto l = launch_facts();
    CommittedView v;
    std::string why;
    CHECK(v.bind(100, l, full_table(), &handed, &why));
    // the committed value, in the snapshot's form: canonical GUID text, 8-hex type, the translated value
    Read r = tree_of(v, "item:Test_Item_Logs");
    CHECK(r.rc == KILN_OK && r.required == 777);
    const json got = json::parse(r.json, nullptr, false);
    CHECK(got == json::parse(R"({"resourceId":")" + kGuidItem + R"(","typeHash":"1a2b3c4d","part":0,"value":{"name":"Test_Item_Logs","stackSize":75,"tags":["wood","fuel"]}})"));
    CHECK(r.json.find(R"({"resourceId":")" + kGuidItem + R"(","typeHash":"1a2b3c4d","part":0,"value":{"name":)") == 0);  // field order kept
    CHECK(r.value == r.json.size() - r.json.find("\"value\":") - 9);
    // the buffer rules of kiln.boot: too small sets required (the NUL counted), a value over the limits is unavailable
    const size_t need = r.json.size() + 1;
    Read small = tree_of(v, "item:Test_Item_Logs", need - 1);
    CHECK(small.rc == KILN_ERR_BUFFER_TOO_SMALL && small.required == need && small.json.empty());
    CHECK(tree_of(v, "item:Test_Item_Logs", need).rc == KILN_OK);
    CHECK(tree_of(v, "item:Test_Item_Logs", 1 << 20, need - 1).rc == KILN_ERR_UNAVAILABLE);
    Read budget = tree_of(v, "item:Test_Item_Logs", 1 << 20, 4u << 20, r.value - 1);
    CHECK(budget.rc == KILN_ERR_UNAVAILABLE && budget.why.find("budget") != std::string::npos);
    CHECK(tree_of(v, "item:Test_Item_Logs", 1 << 20, 4u << 20, r.value).rc == KILN_OK);
    // query forms: a resource by type name or upper-case hex, a skill in upper case: resource_json's lower-case grammar
    f.replies["resource:" + kGuidItem + "/1a2b3c4d/0"] = f.replies["item:Test_Item_Logs"];
    char named[9];
    std::snprintf(named, sizeof named, "%08x", boot_gd::fnv1a32("keen::TestResource"));
    f.replies["resource:" + kGuidItem + "/" + named + "/2"] =
        reply_for(f.granted, kGuidItemGame, boot_gd::fnv1a32("keen::TestResource"), 2, tv_map({{"a", tv_u64(1)}, {"b", tv_u64(2)}}));
    f.replies["skill:0x8efcc472"] = reply_for(f.granted, kGuidItemGame, 0xc0ffee, 0, tv_map({{"cost", tv_u64(2)}, {"tier", tv_u64(1)}}));
    CHECK(tree_of(v, "resource:" + kGuidItem + "/1A2B3C4D/0").rc == KILN_OK && f.queries.back() == "resource:" + kGuidItem + "/1a2b3c4d/0");
    Read byname = tree_of(v, "resource:" + kGuidItem + "/keen::TestResource/2");
    CHECK(byname.rc == KILN_OK && json::parse(byname.json)["value"] == json::parse(R"({"a":1,"b":2})"));
    CHECK(json::parse(byname.json)["typeHash"] == named && json::parse(byname.json)["part"] == 2);
    CHECK(tree_of(v, "skill:0x8EFCC472").rc == KILN_OK && f.queries.back() == "skill:0x8efcc472");
    // names:... are the snapshot's: unavailable, nothing asked
    const size_t asked = f.queries.size();
    for (const char* q : {"names:items", "names:templates/MiningNode", "names:resources/keen::SceneResource"}) {
        Read n = tree_of(v, q);
        CHECK(n.rc == KILN_ERR_UNAVAILABLE && n.why.find("boot snapshot") != std::string::npos);
    }
    CHECK(f.queries.size() == asked && tree_of(v, "bogus").rc == KILN_ERR_INVALID_ARGUMENT);
    // the provider's refusals: never another generation's answer
    CHECK(tree_of(v, "item:Missing").rc == KILN_ERR_NOT_FOUND);
    f.fails["template:Named"] = {KILN_ERR_UNAVAILABLE, ""};
    Read nidx = tree_of(v, "template:Named");
    CHECK(nidx.rc == KILN_ERR_UNAVAILABLE && nidx.why == "kiln-eml cannot answer it from the committed archive generation");
    f.fails["item:Broken"] = {KILN_ERR_INTERNAL, std::string("resource 1: decoding failed\0here", 32)};
    Read fault = tree_of(v, "item:Broken");
    CHECK(fault.rc == KILN_ERR_UNAVAILABLE && fault.why.find("decoding failed") != std::string::npos && f.tokens.empty() && f.tokens_released == 1);
    f.fails["item:Odd"] = {KILN_ERR_INVALID_ARGUMENT, "bad query"};
    CHECK(tree_of(v, "item:Odd").rc == KILN_ERR_UNAVAILABLE && f.tokens.empty());
    f.fails["item:Long"] = {KILN_ERR_INTERNAL, std::string(5000, 'x')};
    Read longer = tree_of(v, "item:Long");
    CHECK(longer.rc == KILN_ERR_UNAVAILABLE && longer.why.size() < 1200 && f.tokens.empty());
    // a reply of another generation, a reply that is no envelope, a value that does not translate, a size that moves
    LaunchFacts other_launch = launch_facts();
    other_launch.launch_id ^= 1;
    f.replies["item:Other"] = reply_for(identity_for(other_launch), kGuidItemGame, 1, 0, tv_u64(1));
    CHECK(tree_of(v, "item:Other").why.find("another archive generation") != std::string::npos);
    f.replies["item:Junk"] = "{\"schema_version\":2}";
    CHECK(tree_of(v, "item:Junk").rc == KILN_ERR_UNAVAILABLE);
    f.replies["item:NoId"] = json{{"schema_version", 1}, {"pin", kiln::boot_view::pin_of(f.granted)}, {"value", tv_u64(1)}}.dump();
    CHECK(tree_of(v, "item:NoId").why.find("names no resource") != std::string::npos);
    f.replies["item:Bad"] = reply_for(f.granted, kGuidItemGame, 1, 0, json{{"tag", "wat"}});
    CHECK(tree_of(v, "item:Bad").why.find("cannot be translated") != std::string::npos);
    f.grow["item:Test_Item_Logs"] = 1;
    CHECK(tree_of(v, "item:Test_Item_Logs").why.find("changed size") != std::string::npos);
    // bytes: read_resource of the same generation
    KilnAssetKey key{};
    key.type = 0x1a2b3c4du;
    f.res[std::string(reinterpret_cast<const char*>(&key), sizeof key)] = std::string("\0raw\0", 5);
    std::string out;
    CHECK(v.bytes(key, 1 << 20, &out, &why) == KILN_OK && out == std::string("\0raw\0", 5));
    CHECK(v.bytes(key, 4, &out, &why) == KILN_ERR_UNAVAILABLE && why.find("larger") != std::string::npos);
    key.part = 9;
    CHECK(v.bytes(key, 1 << 20, &out, &why) == KILN_ERR_NOT_FOUND);
    // revoked under the view (the generation retired): stale from then on, with the reason
    {
        std::lock_guard lk(f.m);
        f.released.insert(*f.live.begin());
        f.live.clear();
    }
    Read rev = tree_of(v, "item:Test_Item_Logs");
    CHECK(rev.rc == KILN_ERR_UNAVAILABLE && rev.why.find("stale") != std::string::npos);
    CHECK(!v.ready(&why) && why.find("stale") != std::string::npos && v.digest().empty());
    const int calls = f.calls;
    CHECK(tree_of(v, "item:Test_Item_Logs").rc == KILN_ERR_UNAVAILABLE && f.calls == calls);
    const int releases = f.releases;
    v.release();  // the stale token is given back once
    CHECK(f.releases == releases + 1);
    pass_line("EML-VIEW-READS kiln.boot's queries through resource_json, its refusals and limits, never another generation", f0);
}

// ---- the translation ----
void translate_case() {
    t_case = "translate";
    const int f0 = t_fail;
    auto tr = [](const json& v, std::string* why = nullptr) {
        ojson out;
        std::string w;
        const bool ok = kiln::boot_view::translate_value(v, &out, &w);
        if (why) *why = w;
        return ok ? json::parse(out.dump()) : json("FAILED");
    };
    CHECK(tr(json{{"tag", "nil"}}).is_null());
    CHECK(tr(json{{"tag", "bool"}, {"value", true}}) == true);
    CHECK(tr(tv_u64(0x8000000000000005ull, "uint64")).get<uint64_t>() == 0x8000000000000005ull);
    CHECK(tr(tv_i64(-42)) == -42);
    CHECK(tr(tv_f32(120.5f)) == 120.5 && tr(tv_f32(0.1f)).get<double>() == static_cast<double>(0.1f));
    CHECK(tr(tv_f32(std::nanf(""))).is_null() && tr(tv_f64(-INFINITY)).is_null() && tr(tv_f64(2.25)) == 2.25);
    CHECK(tr(tv_guid(std::string(32, '0'))) == "none" && tr(tv_guid(kGuidItemGame)) == kGuidItem && tr(tv_guid(kGuidItemGame, "reference")) == kGuidItem);
    CHECK(tr(tv_str("Printable ~ text")) == "Printable ~ text" && tr(tv_str("tab\there")) == "" && tr(tv_str("")) == "" &&
          tr(tv_str(std::string(512, 'a'))) == std::string(512, 'a') && tr(tv_str(std::string(513, 'a'))) == "");
    CHECK(tr(tv_bytes(std::string(17, 'x'))) == "(17 bytes)" && tr(tv_bytes("\x01\x02")) == json::array({1, 2}));
    CHECK(tr(json{{"tag", "optional"}, {"present", false}}).is_null() && tr(json{{"tag", "optional"}, {"present", true}, {"value", tv_u64(3)}}) == 3);
    // a variant: its struct with "$type" first; a keen::Time: seconds; a struct of one scalar: the scalar
    const json variant = {{"tag", "variant"},
                          {"type", "keen::Base"},
                          {"discriminant", "u64:00000000deadbeef"},
                          {"value", tv_map({{"trade", tv_u64(1)}, {"stock", tv_u64(2)}}, "keen::ecs::Trader")}};
    ojson vo;
    std::string why;
    CHECK(kiln::boot_view::translate_value(variant, &vo, &why) && vo.dump() == R"({"$type":"Trader","trade":1,"stock":2})");
    CHECK(tr(tv_map({{"value", tv_i64(1500000000, "sint64")}}, "keen::Time")) == 1.5);
    CHECK(tr(tv_map({{"value", tv_u64(7)}}, "keen::HashKey32")) == 7);
    const json ref = json::object({{"id", kGuidItem}});
    CHECK((tr(tv_map({{"id", tv_guid(kGuidItemGame)}}, "keen::Ref")) == ref));  // a GUID field is no scalar leaf
    CHECK(kiln::boot_view::eml_tree::brief("keen::Array<keen::ecs::X>") == "Array<keen::ecs::X>" && kiln::boot_view::eml_tree::brief("Plain") == "Plain");
    // depth: 12 levels below the root, then "..." (scalars are read at any depth)
    json deep = tv_map({{"x", tv_u64(0)}, {"leaf", tv_u64(13)}});
    for (int i = 12; i >= 1; --i) deep = tv_map({{"x", tv_u64(static_cast<uint64_t>(i))}, {"n", deep}});
    const json d = tr(deep);
    json at = d;
    for (int i = 0; i < 11; ++i) at = at["n"];
    CHECK(at.is_object() && at["x"] == 12 && at["n"] == "...");
    // refusals: a repeated or unnamed field, an unknown tag, a broken scalar, too many nodes
    const json twice = {{"tag", "map"}, {"entries", json::array({{{"key", tv_str("a")}, {"value", tv_u64(1)}}, {{"key", tv_str("a")}, {"value", tv_u64(2)}}})}};
    CHECK(tr(twice, &why) == "FAILED" && why.find("twice") != std::string::npos);
    const json unnamed = {{"tag", "map"}, {"entries", json::array({{{"key", tv_u64(1)}, {"value", tv_u64(1)}}, {{"key", tv_str("b")}, {"value", tv_u64(2)}}})}};
    CHECK(tr(unnamed, &why) == "FAILED" && why.find("field name") != std::string::npos);
    CHECK(tr(json{{"tag", "u64"}, {"value", "u64:12"}}, &why) == "FAILED" && tr(json{{"tag", "f32"}, {"value", "f32:zz000000"}}) == "FAILED");
    CHECK(tr(json{{"tag", "wat"}}) == "FAILED" && tr(json::array()) == "FAILED" && tr(json{{"tag", "variant"}, {"value", tv_u64(1)}}) == "FAILED");
    // nesting: an optional does not count toward the 12 levels, so a chain of them is bounded on its own: 64 deep
    // translates, one past 256 levels refuses (no recursion without a bound)
    auto optional_chain = [](int n) {
        json v = tv_u64(5);
        for (int i = 0; i < n; ++i) {
            json next = json::object();
            next["tag"] = "optional";
            next["present"] = true;
            next["value"] = std::move(v);
            v = std::move(next);
        }
        return v;
    };
    CHECK(tr(optional_chain(64)) == 5);
    CHECK(tr(optional_chain(5000), &why) == "FAILED" && why.find("nests deeper") != std::string::npos);
    std::vector<json> many(100001, json{{"tag", "nil"}});
    CHECK(tr(tv_array(many), &why) == "FAILED" && why.find("100000") != std::string::npos);
    pass_line("EML-TRANSLATE the lossless tree in the snapshot's value form (as far as the tree carries it)", f0);
}

// ---- the lifetime ----
void release_case() {
    t_case = "release";
    const int f0 = t_fail;
    const auto l = launch_facts();
    std::string why;
    {
        // a read in flight: release() waits for it, then gives the view back; no read reaches the provider after it
        FakeArchive f;
        fresh(f);
        CommittedView v;
        CHECK(v.bind(100, l, full_table(), &handed, &why));
        f.hold_ms = 50;
        std::atomic<int> rc{-1};
        std::thread reader([&] { rc = tree_of(v, "item:Test_Item_Logs").rc; });
        while (f.calls.load() == 0) std::this_thread::yield();
        v.release();
        reader.join();
        CHECK(rc == KILN_OK && f.late == 0 && f.released.size() == 1 && f.live.empty());
        f.hold_ms = 0;
        CHECK(tree_of(v, "item:Test_Item_Logs").why.find("released its view") != std::string::npos && f.late == 0);
        const int releases = f.releases;
        v.release();  // idempotent: nothing held, nothing given back twice
        CHECK(f.releases == releases);
        // a new generation: reset, bound again, a new view
        v.reset();
        CHECK(!v.ready(&why) && v.bind(100, l, full_table(), &handed, &why) && f.live.size() == 1);
        v.reset();  // reset releases what it holds
        CHECK(f.live.empty());
    }
    {
        // the commit's release before the binding: the generation's reads are over, bind() acquires nothing
        FakeArchive f;
        fresh(f);
        CommittedView v;
        v.release();
        CHECK(!v.bind(100, l, full_table(), &handed, &why) && why.find("released its view") != std::string::npos && f.acquires == 0);
        CHECK(!v.ready(&why) && tree_of(v, "item:Test_Item_Logs").rc == KILN_ERR_UNAVAILABLE && f.calls == 0);
    }
    {
        // released while bind() waits on the identity source: the view acquired after it is given back at once, never
        // served
        FakeArchive f;
        fresh(f);
        CommittedView v;
        t_source_hold = true;
        t_source_waiting = false;
        bool ok = true;
        std::string bwhy;
        std::thread binder([&] { ok = v.bind(100, l, full_table(), &blocking_handed, &bwhy); });
        while (!t_source_waiting.load()) std::this_thread::yield();
        v.release();
        t_source_hold = false;
        binder.join();
        CHECK(!ok && bwhy.find("released its view") != std::string::npos);
        CHECK(f.live.empty() && tree_of(v, "item:Test_Item_Logs").rc == KILN_ERR_UNAVAILABLE && f.calls == 0);
    }
    {
        // eight threads reading across the release (the TSan build): every answer the committed value or unavailable,
        // never a provider call on the released view
        FakeArchive f;
        fresh(f);
        CommittedView v;
        CHECK(v.bind(100, l, full_table(), &handed, &why));
        std::atomic<bool> go{false};
        std::atomic<int> bad{0}, served{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 8; ++t)
            th.emplace_back([&] {
                while (!go.load()) std::this_thread::yield();
                for (int i = 0; i < 300; ++i) {
                    const Read r = tree_of(v, "item:Test_Item_Logs");
                    if (r.rc == KILN_OK)
                        ++served;
                    else if (r.rc != KILN_ERR_UNAVAILABLE)
                        ++bad;
                    (void)v.digest();
                }
            });
        go = true;
        while (served.load() < 20) std::this_thread::yield();
        v.release();
        for (auto& t : th) t.join();
        CHECK(bad == 0 && f.late == 0 && f.live.empty());
    }
    pass_line("EML-VIEW-RELEASE waits for reads in flight; no read after the release (8 threads)", f0);
}

// ---- kiln.boot over it ----
const KilnBootApiV1& B = kiln::boot_svc::kTable;
KilnStr ks(const std::string& s) { return KilnStr{s.data(), s.size()}; }
std::vector<uint32_t> t_exits;
void fake_exit(uint32_t code) { t_exits.push_back(code); }
fs::path t_root;
constexpr KilnOwner kHost = 7;

struct Plan {
    int collector;
};
KilnResult KILN_CALL ok_prepare(void* user, const KilnBootPrepareInputV1*, KilnBootPreparedV1* out, KilnBootIssueV1*) {
    out->ok = 1;
    out->plan = new Plan{static_cast<int>(reinterpret_cast<intptr_t>(user))};
    out->destroy = [](const void* p) { delete static_cast<const Plan*>(p); };
    return KILN_OK;
}

void reset_boot() {
    using namespace kiln::boot_svc;
    g_decided = g_deferred = g_bound = false;
    g_why.clear();
    g_boot_mods.clear();
    g_scanned.clear();
    g_status = KILN_BOOT_STATUS_COLLECTING;
    g_owners.clear();
    g_by_author.clear();
    g_host = 0;
    g_envelope = nullptr;
    g_envelope_bytes = 0;
    g_issues.clear();
    for (auto& c : g_collectors) c = {};
    if (const Root* r = g_root.exchange(nullptr)) {
        for (const void* p : r->plans) delete static_cast<const Plan*>(p);
        delete r;
    }
    g_self = 0;
    g_exit = &fake_exit;
    g_exit_code = 0;
    t_exits.clear();
    t_caps.clear();
    t_log.clear();
}

// The snapshot of this game build on disk, made before the start: the item with stack size 50.
const std::string kSha = [] {
    std::string s;
    for (int i = 0; i < 32; ++i) s += "ab";
    return s;
}();
const std::string kVersion = "1287369|^/game38/test|2026-01-01T00:00:00Z";
void write_text(const fs::path& f, const std::string& text) {
    fs::create_directories(f.parent_path());
    std::ofstream(f, std::ios::binary) << text;
}
void write_snapshot() {
    t_paths.exe = t_root / "game" / "enshrouded_server.exe";
    t_paths.data = t_root / "game" / "kiln" / "data";
    t_image.sha256 = kSha;
    {
        const uint32_t rel = 0x30, len = static_cast<uint32_t>(kVersion.size());
        std::string kfc(0x10 + rel, '\0');
        std::memcpy(kfc.data(), "KFC4", 4);
        std::memcpy(kfc.data() + 0x10, &rel, 4);
        std::memcpy(kfc.data() + 0x14, &len, 4);
        write_text(t_root / "game" / "enshrouded_server.kfc", kfc + kVersion);
    }
    const fs::path dir = t_paths.data / "boot-gamedata" / kiln::boot_gd::key_of(kSha, kVersion);
    const std::string value = R"({"name":"Test_Item_Logs","stackSize":50,"tags":["wood","fuel"]})";
    for (const char* k : kiln::boot_gd::kKinds) {
        json entries = json::object();
        std::string data;
        if (std::string(k) == "items") entries["Test_Item_Logs"] = json::array({0, value.size(), kGuidItem, "1a2b3c4d", 0}), data = value;
        write_text(dir / (std::string(k) + ".index.json"), json{{"format", 1}, {"kind", k}, {"entries", entries}}.dump());
        write_text(dir / (std::string(k) + ".data"), data);
    }
    write_text(dir / "index.json", json{{"format", 1}, {"key", kiln::boot_gd::key_of(kSha, kVersion)}, {"side", "server"}}.dump());
}

// The kiln-backend sources the test hands kiln.boot.
bool t_kiln = false;
int t_live_asks = 0;
bool fake_kiln_mode(LaunchFacts* out) {
    if (t_kiln) *out = launch_facts();
    return t_kiln;
}
const KilnEmlArchiveApiV1* fake_table() { return full_table(); }
const kiln::boot_view::CommittedSource t_source{&fake_kiln_mode, &fake_table, &handed};
// kiln-eml's capability rows for the groups kiln.boot uses (kiln_eml_archive.h: checked before the table is used),
// available unless a case turns one off: unavailable, held by the client gate, or absent.
const char* const kArchiveRows[] = {"eml.archive.metadata",         "eml.archive.metadata.acquire", "eml.archive.metadata.identity",
                                    "eml.archive.metadata.release", "eml.archive.read",             "eml.archive.read.resource_json"};
std::string t_row_off;
int t_row_mode = 0;  // for t_row_off: 0 unavailable, 1 held, 2 absent
void report_archive_rows() {
    for (const char* r : kArchiveRows) {
        const bool off = t_row_off == r;
        if (off && t_row_mode == 2) continue;
        fake_report(60, ks(r), off && t_row_mode == 0 ? KILN_CAP_UNAVAILABLE : KILN_CAP_AVAILABLE,
                    off && t_row_mode == 0 ? KILN_CAP_REASON_SERVICE_MISSING : KILN_CAP_REASON_NONE, ks(off && t_row_mode == 0 ? "turned off" : ""));
        if (off && t_row_mode == 1)
            fake_report(0, ks(r), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, ks(std::string(KILN_CAP_GATE_TEXT) + "eml-archive"));
    }
}
const KilnAssetsApi* counting_assets() {
    ++t_live_asks;
    return nullptr;
}

// One server boot mod (required when required), deferred mode, every collector registered, bound; its frame open.
KilnOwner deferred_author(bool required = false) {
    reset_boot();
    t_image.side = KILN_SIDE_SERVER;
    t_config.clr = true;
    t_profile = kiln::profile::State{};
    t_profile.mode = KILN_PROFILE_ACTIVE;
    const fs::path dir = t_root / "mods" / "a";
    fs::create_directories(dir);
    std::ofstream(dir / "kiln.mod.json") << json{{"id", "a"}, {"name", "a"}, {"version", "1.0.0"}, {"apiVersion", "0.2"}, {"side", "server"},
                                                {"boot", required ? json("required") : json(true)}}
                                                .dump();
    kiln::profile::Entry e;
    e.id = "a";
    e.path = dir.string();
    e.enabled = true;
    t_profile.mods[KILN_PROFILE_KIND_KILN].push_back(e);
    fake_report(0, ks("mods.boot"), KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, ks(""));
    report_archive_rows();
    kiln::boot_svc::decide(true, "");
    for (int i = 0; i < 7; ++i)
        B.collector(50 + i, ks(kiln::boot_svc::kCollectors[i]), &ok_prepare, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    kiln::boot_svc::bound();
    uint64_t gen = 0;
    B.begin_owner(41, ks("a"), KILN_BOOT_ROLE_DEDICATED | (required ? KILN_BOOT_ROLE_REQUIRED : 0u), &gen);
    return 41;
}
std::string row_text(const char* name) {
    const KilnCapRow* r = shown(name);
    return r ? std::string(r->text.ptr, r->text.len) : std::string("(no row)");
}
uint32_t row_state(const char* name) {
    const KilnCapRow* r = shown(name);
    return r ? r->state : 99;
}
std::string author_read(KilnOwner a, const std::string& q, KilnResult* rc) {
    size_t need = 0;
    char none = 0;
    *rc = B.read_resource(a, ks(q), &none, 0, &need);
    if (*rc != KILN_ERR_BUFFER_TOO_SMALL) return {};
    std::string out(need, '\x5A');
    *rc = B.read_resource(a, ks(q), out.data(), out.size(), &need);
    if (*rc != KILN_OK) return {};
    out.pop_back();
    return out;
}
KilnBootCommitResultV1 commit_now() {
    KilnBootCommitResultV1 c{};
    c.size = sizeof c;
    B.commit(kHost, 1, &c);
    return c;
}

// RED first: an old snapshot of this exact game build (same exe, same .kfc version) holds stack size 50; the startup
// pass committed 75. A kiln-backend start's author reads 75; a hosted start reads the snapshot's 50.
void committed_not_snapshot_case() {
    t_case = "committed-not-snapshot";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    write_snapshot();
    FakeArchive f;
    fresh(f, 75);
    KilnResult rc = KILN_OK;
    // hosted: no kiln backend, the snapshot as before
    use_committed_generation(&t_source);
    t_kiln = false;
    t_identity_calls = 0;
    KilnOwner a = deferred_author();
    const std::string hosted = author_read(a, "item:Test_Item_Logs", &rc);
    CHECK(rc == KILN_OK && json::parse(hosted)["value"]["stackSize"] == 50);
    CHECK(f.acquires == 0 && t_identity_calls == 0);
    // kiln backend: decided pending, bound at bound(), read from the committed generation; the snapshot is never opened
    t_kiln = true;
    t_identity_calls = 0;
    kiln::boot_gd::Reader& snap = g_gamedata;
    snap.open("/nonexistent", kSha, "/nonexistent.kfc", &t_case);  // closed, so an opening shows
    t_case = "committed-not-snapshot";
    a = deferred_author();
    CHECK(!snap.is_open());
    CHECK(row_state("mods.boot.game_data") == KILN_CAP_AVAILABLE && row_text("mods.boot.game_data").find("committed") != std::string::npos);
    CHECK(f.acquires == 1 && f.live.size() == 1 && t_identity_calls == 1);
    const std::string kiln_mode = author_read(a, "item:Test_Item_Logs", &rc);
    CHECK(rc == KILN_OK);
    json k = json::parse(kiln_mode, nullptr, false), h = json::parse(hosted, nullptr, false);
    CHECK(k["value"]["stackSize"] == 75);
    k["value"]["stackSize"] = 50;
    CHECK(k == h);  // the same result in every other field: GUID text, type, part, the value's form
    CHECK(g_owners["a"].read_bytes == json::parse(kiln_mode)["value"].dump().size());
    // names: lists are the snapshot's: unavailable in a kiln-backend start, never the snapshot's answer
    CHECK(author_read(a, "names:items", &rc).empty() && rc == KILN_ERR_UNAVAILABLE);
    // the commit releases the view; no read reaches the provider after it
    B.finish_owner(a, 1, KILN_BOOT_FINISH_ACCEPT);
    CHECK(commit_now().status == KILN_BOOT_STATUS_PUBLISHED);
    CHECK(f.live.empty() && f.released.size() == 1 && f.late == 0);
    CHECK(after_clr(true, nullptr, nullptr) && t_exits.empty());
    // and hosted again in the next start
    t_kiln = false;
    a = deferred_author();
    CHECK(json::parse(author_read(a, "item:Test_Item_Logs", &rc))["value"]["stackSize"] == 50);
    use_committed_generation(nullptr);
    pass_line("EML-BOOT-RED an old snapshot of the same build is not read in a kiln-backend start; hosted reads it as before", f0);
}

// No generation (the hand-off is missing, or refused): unavailable with the reason, never the snapshot, and the
// OnBoot policy: an optional mod is dropped and the start goes on, a required mod stops it (the fake exit adapter).
void no_generation_case() {
    t_case = "no-generation";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    write_snapshot();
    FakeArchive f;
    fresh(f, 75);
    use_committed_generation(&t_source);
    t_kiln = true;
    t_handed_rc = KILN_ERR_UNAVAILABLE;
    t_handed_why = "kiln-eml does not hand this start's committed archive generation to kiln.boot yet";
    g_live_assets = &counting_assets;
    KilnResult rc = KILN_OK;
    for (const bool required : {false, true}) {
        t_case = required ? "no-generation-required" : "no-generation-optional";
        t_handed_rc = KILN_ERR_UNAVAILABLE;
        std::string closed;
        g_gamedata.open("/nonexistent", kSha, "/nonexistent.kfc", &closed);  // closed, so an opening shows
        const KilnOwner a = deferred_author(required);
        CHECK(&base_view() == &g_committed_view);
        CHECK(row_state("mods.boot.game_data") == KILN_CAP_UNAVAILABLE &&
              row_text("mods.boot.game_data") == "game data unavailable: " + t_handed_why);
        CHECK(!g_gamedata.is_open() && f.acquires == 0);
        bool logged = false;
        for (const auto& l : t_log) logged |= l.find("boot snapshot is not read") != std::string::npos && l.find(t_handed_why) != std::string::npos;
        CHECK(logged);
        CHECK(author_read(a, "item:Test_Item_Logs", &rc).empty() && rc == KILN_ERR_UNAVAILABLE);  // not the snapshot's 50
        // the mod's OnBoot fails without its data (Kiln.Host reports it as a failed callback)
        CHECK(B.finish_owner(a, 1, KILN_BOOT_FINISH_CALLBACK_FAILED) == KILN_OK);
        const KilnBootCommitResultV1 c = commit_now();
        if (!required) {
            CHECK(c.status == KILN_BOOT_STATUS_PUBLISHED && after_clr(true, nullptr, nullptr) && t_exits.empty());
        } else {
            CHECK(c.status == KILN_BOOT_STATUS_ABORTED);
            CHECK(!after_clr(true, nullptr, nullptr) && t_exits == std::vector<uint32_t>{KILN_BOOT_EXIT_CODE});
        }
    }
    CHECK(t_live_asks == 0);  // nothing waited on or asked of the game's resources
    // the identity handed but for another launch: unavailable the same way, the reason says which
    t_case = "no-generation-wrong-launch";
    t_handed_rc = KILN_OK;
    t_handed.launch_id[15] ^= 1;
    const KilnOwner a = deferred_author();
    CHECK(row_text("mods.boot.game_data") == "game data unavailable: the committed archive generation belongs to another launch");
    CHECK(author_read(a, "item:Test_Item_Logs", &rc).empty() && rc == KILN_ERR_UNAVAILABLE);
    // the archive table missing: the same
    t_case = "no-generation-no-table";
    static const kiln::boot_view::CommittedSource no_table{&fake_kiln_mode, [] { return static_cast<const KilnEmlArchiveApiV1*>(nullptr); }, &handed};
    use_committed_generation(&no_table);
    fresh(f, 75);
    deferred_author();
    CHECK(row_text("mods.boot.game_data").find("offers no kiln.eml.archive table") != std::string::npos);
    use_committed_generation(nullptr);
    g_live_assets = &assets_service;
    t_kiln = false;
    pass_line("EML-BOOT-NO-GENERATION unavailable with the reason, no snapshot fallback, optional dropped, required stops the start", f0);
}

// ---- the production identity source, kiln.eml.startup's session-free published_identity ----
// A fake kiln.eml.startup table as kiln-eml's early export serves it (kiln_eml_startup.h's real layout): its
// published_identity hands t_handed (the identity the fake archive granted) or refuses with t_published_rc.
KilnEmlStartupApiV1 t_startup{};
const KilnEmlStartupApiV1* t_startup_served = nullptr;
uint32_t t_startup_asked = 0;
int t_published_calls = 0;
KilnResult t_published_rc = KILN_OK;
KilnEmlStartupKeyV1 t_key_seen{};
uint32_t t_size_seen = 0;
KilnResult KILN_CALL fs_published(const KilnEmlStartupKeyV1* key, KilnEmlArchiveIdentityV1* out) {
    ++t_published_calls;
    t_key_seen = *key;
    t_size_seen = out->size;
    if (t_published_rc != KILN_OK) return t_published_rc;
    const uint32_t size = out->size;
    *out = t_handed;
    out->size = size;
    return KILN_OK;
}
const KilnEmlStartupApiV1* KILN_CALL fs_getter(uint32_t min_version) {
    t_startup_asked = min_version;
    return min_version <= KILN_EML_STARTUP_VERSION ? t_startup_served : nullptr;
}
void fresh_startup() {
    t_startup = KilnEmlStartupApiV1{};
    t_startup.header.size = sizeof t_startup;
    t_startup.header.version = KILN_EML_STARTUP_VERSION;
    t_startup.published_identity = &fs_published;
    t_startup_served = &t_startup;
    t_published_rc = KILN_OK;
    t_published_calls = 0;
    t_startup_asked = 0;
}
// boot_archive_eml.cpp's identity source with the getter it found (here the fake's)
KilnResult KILN_CALL via_startup(const LaunchFacts& l, KilnEmlArchiveIdentityV1* out, std::string* why) {
    return kiln::boot_view::published_identity_of(&fs_getter, l, out, why);
}
const kiln::boot_view::CommittedSource t_startup_source{&fake_kiln_mode, &fake_table, &via_startup};

void published_identity_case() {
    t_case = "published-identity";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    FakeArchive f;
    fresh(f, 75);
    fresh_startup();
    const auto l = launch_facts();
    std::string why;
    // the call: version 1 asked, the session-free key {56, 0, kiln.launch's launch id and plan hash, session 0}, the
    // caller's record size, and exactly the identity handed (its size kept)
    KilnEmlArchiveIdentityV1 got{};
    got.size = sizeof got;
    CHECK(kiln::boot_view::published_identity_of(&fs_getter, l, &got, &why) == KILN_OK && why.empty());
    CHECK(t_startup_asked == KILN_EML_STARTUP_VERSION && t_published_calls == 1);
    CHECK(t_key_seen.size == 56 && t_key_seen.flags == 0 && t_key_seen.launch_id == kLaunch && t_key_seen.session == 0 &&
          std::memcmp(t_key_seen.plan_sha256, l.plan_sha256.data(), 32) == 0);
    CHECK(t_size_seen == sizeof got && std::memcmp(&got, &t_handed, sizeof got) == 0);
    // every refusal: unavailable (or the provider's result) with its reason, the record untouched
    struct Refusal {
        const char* name;
        std::function<void()> set;
        KilnResult rc;
        const char* reason;
        int calls;
    };
    const Refusal refusals[] = {
        {"no getter", [] { t_startup_served = nullptr; }, KILN_ERR_UNAVAILABLE, "offers no kiln.eml.startup table", 0},
        {"48-byte table", [] { t_startup.header.size = 48; }, KILN_ERR_UNAVAILABLE, "no published_identity", 0},
        {"55-byte table", [] { t_startup.header.size = 55; }, KILN_ERR_UNAVAILABLE, "no published_identity", 0},
        {"version 0", [] { t_startup.header.version = 0; }, KILN_ERR_UNAVAILABLE, "no published_identity", 0},
        {"null member", [] { t_startup.published_identity = nullptr; }, KILN_ERR_UNAVAILABLE, "no published_identity", 0},
        {"no grant", [] { t_published_rc = KILN_ERR_UNAVAILABLE; }, KILN_ERR_UNAVAILABLE, "granted no archive generation", 1},
        {"key refused", [] { t_published_rc = KILN_ERR_INVALID_ARGUMENT; }, KILN_ERR_INVALID_ARGUMENT, "launch key", 1},
        {"fault", [] { t_published_rc = KILN_ERR_INTERNAL; }, KILN_ERR_INTERNAL, "could not hand", 1},
    };
    for (const auto& r : refusals) {
        t_case = std::string("published-identity-") + r.name;
        fresh_startup();
        r.set();
        KilnEmlArchiveIdentityV1 out{};
        out.size = sizeof out;
        out.generation = 0xdead;
        why.clear();
        CHECK(kiln::boot_view::published_identity_of(&fs_getter, l, &out, &why) == r.rc);
        CHECK(why.find(r.reason) != std::string::npos && t_published_calls == r.calls && out.generation == 0xdead);
        // the view binds nothing with that reason, acquires nothing, and reads nothing (never another base's answer)
        const int acquires = f.acquires;
        CommittedView v;
        std::string bind_why;
        CHECK(!v.bind(100, l, full_table(), &via_startup, &bind_why) && bind_why == why && f.acquires == acquires);
        CHECK(tree_of(v, "item:Test_Item_Logs").rc == KILN_ERR_UNAVAILABLE);
        v.release();
    }
    // the getter itself: null
    t_case = "published-identity-null-getter";
    why.clear();
    CHECK(kiln::boot_view::published_identity_of(nullptr, l, &got, &why) == KILN_ERR_UNAVAILABLE &&
          why.find("offers no kiln.eml.startup table") != std::string::npos);
    // a short record is refused before the call
    t_case = "published-identity-short";
    fresh_startup();
    KilnEmlArchiveIdentityV1 short_id{};
    short_id.size = sizeof short_id - 1;
    CHECK(kiln::boot_view::published_identity_of(&fs_getter, l, &short_id, &why) == KILN_ERR_INVALID_ARGUMENT && t_published_calls == 0);
    // kiln.boot end to end over it: decided pending, bound at bound(), the author reads the committed 75 and never the
    // snapshot's 50; with no grant: unavailable with the reason, no snapshot
    t_case = "published-identity-boot";
    write_snapshot();
    fresh(f, 75);
    fresh_startup();
    use_committed_generation(&t_startup_source);
    t_kiln = true;
    std::string closed;
    g_gamedata.open("/nonexistent", kSha, "/nonexistent.kfc", &closed);  // closed, so an opening shows
    KilnResult rc = KILN_OK;
    KilnOwner a = deferred_author();
    CHECK(!g_gamedata.is_open() && t_published_calls == 1 && f.acquires == 1);
    CHECK(row_state("mods.boot.game_data") == KILN_CAP_AVAILABLE);
    CHECK(json::parse(author_read(a, "item:Test_Item_Logs", &rc))["value"]["stackSize"] == 75 && rc == KILN_OK);
    B.finish_owner(a, 1, KILN_BOOT_FINISH_ACCEPT);
    CHECK(commit_now().status == KILN_BOOT_STATUS_PUBLISHED && f.live.empty());
    t_case = "published-identity-boot-no-grant";
    fresh(f, 75);
    fresh_startup();
    t_published_rc = KILN_ERR_UNAVAILABLE;
    g_gamedata.open("/nonexistent", kSha, "/nonexistent.kfc", &closed);  // closed, so an opening shows
    const int acquired = f.acquires;
    a = deferred_author();
    CHECK(row_state("mods.boot.game_data") == KILN_CAP_UNAVAILABLE &&
          row_text("mods.boot.game_data").find("granted no archive generation") != std::string::npos);
    CHECK(!g_gamedata.is_open() && f.acquires == acquired && t_published_calls == 1);
    CHECK(author_read(a, "item:Test_Item_Logs", &rc).empty() && rc == KILN_ERR_UNAVAILABLE);
    B.finish_owner(a, 1, KILN_BOOT_FINISH_CALLBACK_FAILED);
    CHECK(commit_now().status == KILN_BOOT_STATUS_PUBLISHED && after_clr(true, nullptr, nullptr) && t_exits.empty());
    use_committed_generation(nullptr);
    t_kiln = false;
    pass_line("EML-BOOT-PUBLISHED-IDENTITY kiln.eml.startup's session-free identity binds the committed generation; every refusal is unavailable with its reason", f0);
}

// kiln_eml_archive.h: a consumer checks the group and member rows before it uses the table; a held, off or absent row
// refuses. Then kiln.boot binds nothing: game data unavailable with the row named, no identity asked, nothing acquired.
void caps_rows_case() {
    t_case = "caps-rows";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    use_committed_generation(&t_source);
    t_kiln = true;
    KilnResult rc = KILN_OK;
    for (const char* row : kArchiveRows)
        for (int mode = 0; mode < 3; ++mode) {
            FakeArchive f;
            fresh(f, 75);
            t_row_off = row;
            t_row_mode = mode;
            t_identity_calls = 0;
            const KilnOwner a = deferred_author();
            t_case = std::string("caps-rows-") + row + (mode == 0 ? "-off" : mode == 1 ? "-held" : "-absent");
            CHECK(row_state("mods.boot.game_data") == KILN_CAP_UNAVAILABLE &&
                  row_text("mods.boot.game_data").find(std::string("kiln.caps row ") + row + " ") != std::string::npos);
            CHECK(f.acquires == 0 && t_identity_calls == 0 && f.live.empty());
            CHECK(author_read(a, "item:Test_Item_Logs", &rc).empty() && rc == KILN_ERR_UNAVAILABLE && f.calls == 0);
        }
    t_case = "caps-rows";
    t_row_off.clear();
    t_row_mode = 0;
    {
        // every row available: bound as before
        FakeArchive f;
        fresh(f, 75);
        const KilnOwner a = deferred_author();
        CHECK(row_state("mods.boot.game_data") == KILN_CAP_AVAILABLE && f.acquires == 1);
        CHECK(json::parse(author_read(a, "item:Test_Item_Logs", &rc))["value"]["stackSize"] == 75);
        B.finish_owner(a, 1, KILN_BOOT_FINISH_ACCEPT);
        CHECK(commit_now().status == KILN_BOOT_STATUS_PUBLISHED && f.live.empty());
    }
    use_committed_generation(nullptr);
    t_kiln = false;
    pass_line("EML-BOOT-CAPS-ROWS a held, off or absent archive row: no binding, game data unavailable naming the row", f0);
}

// decide() again (a host test's second start): the view of the first is released before the next binds.
void second_decide_case() {
    t_case = "second-decide";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    FakeArchive f;
    fresh(f, 75);
    use_committed_generation(&t_source);
    t_kiln = true;
    deferred_author();
    CHECK(f.live.size() == 1);
    deferred_author();
    CHECK(f.live.size() == 1 && f.released.size() == 1 && f.acquires == 2);
    // without deferred mode nothing is bound
    reset_boot();
    t_profile = kiln::profile::State{};
    kiln::boot_svc::decide(true, "");
    kiln::boot_svc::bound();
    CHECK(f.acquires == 2 && f.live.empty());
    // and its row says so: no row left pending for a pass that has run
    CHECK(row_state("mods.boot.game_data") == KILN_CAP_UNAVAILABLE && row_text("mods.boot.game_data").find("no boot phase in this process") != std::string::npos);
    use_committed_generation(nullptr);
    t_kiln = false;
    pass_line("EML-BOOT-SECOND-DECIDE one view per generation; no binding without deferred mode", f0);
}
}  // namespace

int main(int argc, char** argv) {
    t_root = fs::path(argc > 1 ? argv[1] : ".") / "boot-archive-eml-test";
    std::error_code ec;
    fs::remove_all(t_root, ec);
    fs::create_directories(t_root);
    t_paths.mods = t_root / "no-dropins";
    bind_case();
    reads_case();
    translate_case();
    release_case();
    committed_not_snapshot_case();
    no_generation_case();
    published_identity_case();
    caps_rows_case();
    second_decide_case();
    reset_boot();
    kiln::boot_svc::g_committed_view.reset();
    fs::remove_all(t_root, ec);
    std::printf("BOOT ARCHIVE EML TESTS %d PASS %d FAIL\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
