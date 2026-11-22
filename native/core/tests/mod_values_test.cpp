// SPDX-License-Identifier: MIT
// Host test of kiln.mod-values v1 (src/services/mod_values.cpp, abi/services/kiln_mod_values.h) and the Kiln-internal
// kiln.internal.mod-values.backend v1 (src/mod_values_host.h) against the REAL owner registry (src/registry.cpp) and a
// stub backend in place of Kiln.ModValues: owner namespaces (a managed mod's manifest id only for an owner Kiln.Host
// made), the error order, the one error rule (a refusal is KILN_OK with ok 0), Before/After flags, compare-and-set
// arguments, subscriptions (scope, filter, read policy at dispatch, faults, unsubscribe waits), and owner revoke
// (subscriptions gone, the definitions that owner made retired with its handle as the backend's token).
// Run by test-mod-values.sh (ASan/UBSan). The fault guard, the log and the service registrar are stubs.
#include <windows.h>  // tests/stubs/windows.h

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/registry.cpp"
#include "../src/services/mod_values.cpp"

namespace {
int g_fail = 0, g_pass = 0;
std::vector<std::string> g_log;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_pass : ++g_fail;
}
std::vector<kiln::svc::Service*>& services() {
    static std::vector<kiln::svc::Service*> v;
    return v;
}
}  // namespace

namespace kiln {
namespace log {
void writef(int level, const char*, const char* fmt, ...) {
    if (level > KILN_LOG_INFO) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log.push_back(buf);
}
}  // namespace log
namespace guard {
// The product guard is a structured-exception handler; here a C++ exception stands for the fault.
bool task(KilnTaskFn fn, void* user, unsigned long* code) {
    try {
        fn(user);
        return true;
    } catch (...) {
        if (code) *code = 0xC0000005;
        return false;
    }
}
}  // namespace guard
namespace hooks::seh {
bool call_owner_retiring(void(KILN_CALL* fn)(KilnOwner, void*), KilnOwner retiring, void* user, unsigned long*) {
    fn(retiring, user);
    return true;
}
}  // namespace hooks::seh
namespace svc {
Registrar::Registrar(Service* s) { services().push_back(s); }
}  // namespace svc
}  // namespace kiln

namespace reg = kiln::reg;

namespace {

KilnStr S(const char* s) { return KilnStr{s, std::strlen(s)}; }

const kiln::svc::Service* service(const char* name) {
    for (auto* s : services())
        if (std::string(s->name) == name) return s;
    return nullptr;
}

bool logged(const char* part) {
    for (const auto& l : g_log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

// ---- the stub backend ----------------------------------------------------------------------------------------------
struct Stub {
    int calls = 0;
    std::string ns, key;
    KilnOwner owner = 0;  // describe/define: the owner handle the core passed (the backend's token)
    uint32_t op = 99;
    int64_t amount = 0;
    const KilnModValueVersion* expected = nullptr;
    KilnModValueVersion expected_copy{};
    uint64_t request_id = 0;
    uint32_t def_kind = 99;
    std::vector<std::pair<std::string, KilnOwner>> retired;  // (namespace, owner) per retire call
    // what the next call answers
    KilnResult rc = KILN_OK;
    uint32_t ok = 1, reason = 0, flags = KMV_AFTER;
    int64_t before_value = 7, after_value = 11;
} g_stub;

void answer(KilnModValueResult* out) {
    out->ok = g_stub.ok;
    out->reason = g_stub.reason;
    out->flags = g_stub.flags;
    out->before.kind = KMV_CURRENCY;
    out->before.value = g_stub.before_value;
    out->before.value_revision = 1;
    out->after.kind = KMV_CURRENCY;
    out->after.value = g_stub.after_value;
    out->after.value_revision = 2;
    out->after.world_rfc4122[0] = 0xab;
    out->character.account = 0xdead;  // the core replaces it with the caller's character (read, change)
}

std::string str(KilnStr s) { return std::string(s.ptr ? s.ptr : "", s.len); }

KilnResult KILN_CALL st_describe(KilnStr ns, KilnOwner owner, const KilnModValueDef* def, KilnModDefinitionResult* out) {
    ++g_stub.calls;
    g_stub.ns = str(ns);
    g_stub.owner = owner;
    g_stub.key = str(def->local_id);
    g_stub.def_kind = def->kind;
    if (g_stub.rc != KILN_OK) return g_stub.rc;
    out->ok = g_stub.ok;
    out->reason = g_stub.reason;
    return KILN_OK;
}
KilnResult KILN_CALL st_read(KilnStr ns, const KilnModCharacter*, KilnStr key, KilnModValueResult* out) {
    ++g_stub.calls;
    g_stub.ns = str(ns);
    g_stub.key = str(key);
    if (out->size != sizeof(KilnModValueResult)) return KILN_ERR_INTERNAL;
    if (g_stub.rc != KILN_OK) return g_stub.rc;
    answer(out);
    return KILN_OK;
}
KilnResult KILN_CALL st_change(KilnStr ns, const KilnModCharacter*, KilnStr key, uint32_t op, int64_t amount,
                               const KilnModValueVersion* expected, uint64_t request_id, KilnModValueResult* out) {
    ++g_stub.calls;
    g_stub.ns = str(ns);
    g_stub.key = str(key);
    g_stub.op = op;
    g_stub.amount = amount;
    g_stub.expected = expected;
    if (expected) g_stub.expected_copy = *expected;
    g_stub.request_id = request_id;
    if (g_stub.rc != KILN_OK) return g_stub.rc;
    answer(out);
    return KILN_OK;
}
KilnResult KILN_CALL st_read_synced(KilnStr ns, KilnStr key, KilnModValueResult* out) {
    ++g_stub.calls;
    g_stub.ns = str(ns);
    g_stub.key = str(key);
    if (g_stub.rc != KILN_OK) return g_stub.rc;
    answer(out);
    out->character.account = 0x77;  // read_synced: the backend names the local character
    return KILN_OK;
}
void KILN_CALL st_retire(KilnStr ns, KilnOwner owner) { g_stub.retired.emplace_back(str(ns), owner); }

void reset_stub() {
    const auto retired = g_stub.retired;
    g_stub = Stub{};
    g_stub.retired = retired;
}

// ---- subscribers ---------------------------------------------------------------------------------------------------
struct Seen {
    int calls = 0;
    std::string key;
    uint32_t kind = 99, scope = 99, flags = 99;
    int64_t after = 0;
    uint64_t request_id = 0;
    bool do_throw = false;
    KilnHandle unsubscribe_self = 0;
    const KilnModValuesApi* api = nullptr;
};
void KILN_CALL on_change(const KilnModValueChange* c, void* user) {
    auto* s = static_cast<Seen*>(user);
    ++s->calls;
    s->key = str(c->qualified_key);
    s->kind = c->event_kind;
    s->scope = c->event_scope;
    s->flags = c->result.flags;
    s->after = c->result.after.value;
    s->request_id = c->request_id;
    if (s->unsubscribe_self) s->api->unsubscribe(s->unsubscribe_self);
    if (s->do_throw) throw std::runtime_error("subscriber fault");
}

KilnModValueDef def_of(const char* id, uint32_t kind = KMV_CURRENCY) {
    KilnModValueDef d{};
    d.size = sizeof d;
    d.kind = kind;
    d.persistence = KMV_WORLD;
    d.version = 1;
    d.carry_epoch = 1;
    d.read_policy = KMV_OWNER_ONLY;
    d.sync_policy = KMV_SYNC_OWNER_MOD;
    d.local_id = S(id);
    d.label_json = S("{\"en\": \"Coins\"}");
    d.maximum = 1000000;
    return d;
}

KilnModValueResult result_buf() {
    KilnModValueResult r;
    std::memset(&r, 0x5a, sizeof r);  // garbage the core must not leave behind
    r.size = sizeof r;
    return r;
}

bool payload_zero(const KilnModValueResult& r) {
    KilnModValueResult z{};
    z.size = r.size;
    z.before.size = z.after.size = sizeof(KilnModValue);
    return std::memcmp(&r, &z, sizeof r) == 0;
}

bool zero_value(const KilnModValue& v) {
    KilnModValue z{};
    z.size = sizeof z;
    return std::memcmp(&v, &z, sizeof v) == 0;
}

KilnModValueChange event(const char* key, uint32_t scope, uint32_t kind = KMV_CHANGED) {
    KilnModValueChange c{};
    c.size = sizeof c;
    c.event_kind = kind;
    c.event_scope = scope;
    c.request_id = 42;
    c.character.account = 5;
    c.character.guid_rfc4122[15] = 1;
    c.qualified_key = S(key);
    c.result.size = sizeof c.result;
    c.result.ok = 1;
    c.result.flags = KMV_BEFORE | KMV_AFTER;
    c.result.before.size = c.result.after.size = sizeof(KilnModValue);
    c.result.after.value = 99;
    return c;
}

KilnModValueExport exported(uint32_t read, uint32_t sync, const KilnStr* mods = nullptr, uint32_t n = 0) {
    KilnModValueExport x{};
    x.size = sizeof x;
    x.read_policy = read;
    x.sync_policy = sync;
    x.read_mods = mods;
    x.read_mod_count = n;
    return x;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line out before a sanitizer report
    const auto* svc = service(KILN_MOD_VALUES_SERVICE);
    check(svc != nullptr, "kiln.mod-values is registered as an in-core service");
    if (!svc) return 1;
    check(std::string_view(KILN_MOD_VALUES_BACKEND_SERVICE) == "kiln.internal.mod-values.backend" &&
              !service(KILN_MOD_VALUES_BACKEND_SERVICE) && !service("kiln.mod-values.backend") && services().size() == 1,
          "the backend table is Kiln-internal (kiln.internal.mod-values.backend) and no in-core service: no svc::find, "
          "so neither the plugins' nor the services' view resolves it (api.cpp gives it to the managed host's view only)");
    check(kiln::mod_values::backend_table() == nullptr, "before the facade's init: no backend table for the host either");
    check(svc->version == 1, "kiln.mod-values v1");
    check(svc->init && svc->init(), "the facade's init subscribes to owner revokes");
    const auto* api = static_cast<const KilnModValuesApi*>(svc->table);
    const auto* be = static_cast<const KilnModValuesBackendApi*>(kiln::mod_values::backend_table());
    check(be != nullptr, "after it: the managed host's view gets the backend table");
    if (!be) return 1;
    check(api->header.size == sizeof(KilnModValuesApi) && api->header.version == KILN_MOD_VALUES_VERSION &&
              be->header.size == sizeof(KilnModValuesBackendApi) && be->header.version == KILN_MOD_VALUES_BACKEND_VERSION &&
              sizeof(KilnModValuesApi) == 72 && sizeof(KilnModValuesBackendApi) == 32 && KILN_MOD_VALUES_BACKEND_VERSION == 1,
          "the tables' headers carry their sizes (72, 32) and version 1");

    const KilnOwner plugin = reg::owner_create("plugin:my-mod.dll");
    const KilnOwner fishing = reg::owner_create_managed_host("mod:fishing");  // what Kiln.Host makes for a managed mod
    const KilnOwner upper = reg::owner_create_managed_host("mod:Fishing");
    const KilnOwner spoof = reg::owner_create("mod:fishing");  // a native owner that merely names itself like one
    const KilnOwner reserved = reg::owner_create_managed_host("mod:native.0123");
    const KilnOwner host_self = reg::owner_create_managed_host("host");

    // ---- owner_namespace ----
    {
        uint32_t need = 0;
        char buf[80];
        std::memset(buf, 'x', sizeof buf);
        const char* native_ns = "native.979f28a72f2a8c18d48f73f0fe910c21";  // sha256("plugin:my-mod.dll")[:32]
        check(api->owner_namespace(plugin, nullptr, 0, &need) == KILN_ERR_BUFFER_TOO_SMALL && need == 40,
              "owner_namespace with no buffer: BUFFER_TOO_SMALL, required 40 (39 bytes and the NUL)");
        check(api->owner_namespace(plugin, buf, 39, &need) == KILN_ERR_BUFFER_TOO_SMALL && need == 40 &&
                  std::all_of(buf, buf + sizeof buf, [](char c) { return c == 'x'; }),
              "one byte too small: BUFFER_TOO_SMALL, only required written, never a truncated namespace");
        check(api->owner_namespace(plugin, buf, 40, &need) == KILN_OK && need == 40 && std::strcmp(buf, native_ns) == 0 &&
                  buf[40] == 'x',
              "a native owner: native. + the first 32 hex digits of the SHA-256 of its exact registry name");
        check(api->owner_namespace(fishing, buf, sizeof buf, &need) == KILN_OK && std::strcmp(buf, "fishing") == 0 && need == 8,
              "a managed owner mod:<id>: its manifest id");
        check(api->owner_namespace(upper, buf, sizeof buf, &need) == KILN_OK &&
                  std::strcmp(buf, "native.8ad4adb6acbefada3d9d62add2f398d7") == 0,
              "mod:<not a manifest id> (uppercase) is a native owner, hashed");
        check(reg::owner_managed_host(fishing) && !reg::owner_managed_host(spoof) && !reg::owner_managed_host(plugin) &&
                  !reg::owner_managed_host(0),
              "the registry knows which owners Kiln.Host made");
        check(api->owner_namespace(spoof, buf, sizeof buf, &need) == KILN_OK &&
                  std::strcmp(buf, "native.4606c0f0d6e4229f95fd79b25868b866") == 0,
              "a native owner named \"mod:fishing\" is not the managed mod: native. + the hash of its name, never \"fishing\"");
        check(api->owner_namespace(reserved, buf, sizeof buf, &need) == KILN_OK &&
                  std::strcmp(buf, "native.8a1c0801dff9c26d2a0e406f1740d36d") == 0,
              "\"native.\" is reserved for hashed namespaces: even a host owner mod:native.<x> is hashed (Kiln.Host refuses such ids)");
        check(api->owner_namespace(host_self, buf, sizeof buf, &need) == KILN_OK &&
                  std::strcmp(buf, "native.4740ae6347b0172c01254ff55bae5aff") == 0,
              "a host owner that is not mod:<id> (the host's own) is hashed");
        check(api->owner_namespace(0, buf, sizeof buf, &need) == KILN_OK &&
                  std::strcmp(buf, "native.cdfe9ad58b46226d63eb322dacd35f4f") == 0,
              "owner 0 (the core, \"kiln\") is a native owner");
        check(api->owner_namespace(plugin, buf, sizeof buf, nullptr) == KILN_ERR_INVALID_ARGUMENT &&
                  api->owner_namespace(plugin, nullptr, 8, &need) == KILN_ERR_INVALID_ARGUMENT,
              "owner_namespace: no required pointer, or no buffer with a capacity: INVALID_ARGUMENT");
        check(api->owner_namespace(987654, buf, sizeof buf, &need) == KILN_ERR_REVOKED,
              "owner_namespace of an owner that does not exist: REVOKED");
    }

    const KilnModCharacter who{0x1111222233334444ull, {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
                                                       0xbb, 0xcc, 0xdd, 0xee, 0xff}};
    const KilnModValueDef coins = def_of("coins");

    // ---- backend absent ----
    {
        KilnModDefinitionResult d{sizeof d, 9, 9, 9};
        check(api->describe(fishing, &coins, &d) == KILN_ERR_UNAVAILABLE && d.ok == 0 && d.reason == 0 && d.size == sizeof d,
              "backend absent: describe is UNAVAILABLE (payload zero)");
        d = {sizeof d, 9, 9, 9};
        check(api->define(fishing, &coins, &d) == KILN_ERR_UNAVAILABLE, "backend absent: define is UNAVAILABLE");
        auto r = result_buf();
        check(api->read(fishing, &who, S("fishing:coins"), &r) == KILN_ERR_UNAVAILABLE && payload_zero(r),
              "backend absent: read is UNAVAILABLE, the result's payload zero");
        r = result_buf();
        check(api->change(fishing, &who, S("fishing:coins"), KMV_ADD, 5, 0, nullptr, 0, &r) == KILN_ERR_UNAVAILABLE &&
                  payload_zero(r),
              "backend absent: change is UNAVAILABLE");
        r = result_buf();
        check(api->read_synced(fishing, S("fishing:coins"), &r) == KILN_ERR_UNAVAILABLE, "backend absent: read_synced is UNAVAILABLE");
        check(api->read(fishing, nullptr, S("fishing:coins"), &r) == KILN_ERR_INVALID_ARGUMENT,
              "an invalid argument is reported before the missing backend");
        check(be->wanted() == 0, "no subscriber: wanted() is 0");
    }

    // ---- bind ----
    KilnModValuesBackend stub{};
    stub.size = sizeof stub;
    stub.describe = &st_describe;
    stub.define = &st_describe;
    stub.read = &st_read;
    stub.change = &st_change;
    stub.read_synced = &st_read_synced;
    stub.retire = &st_retire;
    {
        KilnModValuesBackend half = stub;
        half.change = nullptr;
        check(be->bind(nullptr) == KILN_ERR_INVALID_ARGUMENT && be->bind(&half) == KILN_ERR_INVALID_ARGUMENT,
              "bind refuses no table and a table with a function missing");
        KilnModValuesBackend small = stub;
        small.size = 48;
        KilnModValuesBackend big = stub;
        big.size = sizeof big + 8;
        check(be->bind(&small) == KILN_ERR_INVALID_ARGUMENT && be->bind(&big) == KILN_ERR_INVALID_ARGUMENT,
              "bind takes exactly its own build's table: smaller or larger than v1 is refused (Kiln-internal, not append-only)");
        check(be->bind(&stub) == KILN_OK, "the backend binds");
        check(be->bind(&stub) == KILN_ERR_INVALID_ARGUMENT && logged("bound already"), "a second bind is refused (logged)");
    }

    // ---- describe / define ----
    {
        reset_stub();
        KilnModDefinitionResult d{sizeof d, 9, 9, 9};
        check(api->describe(plugin, &coins, &d) == KILN_OK && d.ok == 1 && d.reason == 0 &&
                  g_stub.ns == "native.979f28a72f2a8c18d48f73f0fe910c21" && g_stub.key == "coins" && g_stub.owner == plugin,
              "describe reaches the backend with the owner's namespace, its owner handle (the token) and the local id");
        reset_stub();
        g_stub.ok = 0;
        g_stub.reason = KMV_DEFINITION_CONFLICT;
        d = {sizeof d, 9, 9, 9};
        check(api->define(fishing, &coins, &d) == KILN_OK && d.ok == 0 && d.reason == KMV_DEFINITION_CONFLICT &&
                  g_stub.ns == "fishing" && g_stub.owner == fishing,
              "a refused define is KILN_OK with ok 0 and the reason (DEFINITION_CONFLICT is 15)");
        reset_stub();
        g_stub.rc = KILN_ERR_WRONG_THREAD;
        d = {sizeof d, 9, 9, 9};
        check(api->define(fishing, &coins, &d) == KILN_ERR_WRONG_THREAD && d.ok == 0 && d.reason == 0,
              "the backend's WRONG_THREAD comes through, the payload zero");
        reset_stub();
        g_stub.ok = 0;
        g_stub.reason = KMV_NONE;
        d = {sizeof d, 9, 9, 9};
        check(api->describe(fishing, &coins, &d) == KILN_ERR_INTERNAL, "a refusal without a reason is the backend's fault: INTERNAL");
        KilnModDefinitionResult big[2];
        std::memset(big, 0x5a, sizeof big);
        big[0].size = sizeof big;
        reset_stub();
        check(api->describe(fishing, &coins, big) == KILN_OK && big[0].ok == 1 && big[0].size == sizeof big &&
                  big[1].ok == 0x5a5a5a5a,
              "a larger out record: size kept, bytes past v1 untouched");
    }

    // ---- invalid definitions never reach the backend ----
    {
        reset_stub();
        int bad = 0, n = 0;
        auto try_def = [&](KilnModValueDef d) {
            ++n;
            KilnModDefinitionResult r{sizeof r, 0, 0, 0};
            if (api->describe(fishing, &d, &r) == KILN_ERR_INVALID_ARGUMENT) ++bad;
        };
        KilnModValueDef d = coins;
        d.size = 136; try_def(d);
        d = coins; d.kind = 3; try_def(d);
        d = coins; d.persistence = 2; try_def(d);
        d = coins; d.read_policy = 3; try_def(d);
        d = coins; d.sync_policy = 3; try_def(d);
        d = coins; d.flags = 16; try_def(d);
        d = coins; d.persistence = KMV_CARRIED; d.flags = KMV_TRUST_IMPORTS; try_def(d);  // currency + carried
        d = def_of("lvl", KMV_EXPERIENCE);
        const int64_t t[] = {0, 100, 400};
        d.xp_thresholds = t; d.threshold_count = 3; d.persistence = KMV_CARRIED; try_def(d);  // carried without trust
        d = coins; d.read_mods = nullptr; d.read_mod_count = 0; d.local_id = S("Coins"); try_def(d);
        d = coins; d.local_id = S("1coins"); try_def(d);
        d = coins; d.local_id = S(""); try_def(d);
        KilnStr mods[] = {S("other")};
        d = coins; d.read_mods = mods; d.read_mod_count = 1; try_def(d);  // read_mods outside READ_LIST
        d = coins; d.read_policy = KMV_READ_LIST; d.read_mods = nullptr; d.read_mod_count = 1; try_def(d);
        KilnStr bad_mod[] = {S("Other")};
        d = coins; d.read_policy = KMV_READ_LIST; d.read_mods = bad_mod; d.read_mod_count = 1; try_def(d);
        d = def_of("lvl", KMV_EXPERIENCE); try_def(d);  // experience without thresholds
        const int64_t t1[] = {5, 10};
        d.xp_thresholds = t1; d.threshold_count = 2; try_def(d);  // first is not 0
        const int64_t t2[] = {0, 10, 10};
        d.xp_thresholds = t2; d.threshold_count = 3; try_def(d);  // not increasing
        d.xp_thresholds = nullptr; d.threshold_count = 2; try_def(d);  // count without pointer
        d = coins; d.xp_thresholds = t; d.threshold_count = 3; try_def(d);  // thresholds on a currency
        d = coins; d.label_json = S(""); try_def(d);
        d = coins; d.label_json = KilnStr{"\xff\xfe", 2}; try_def(d);  // not UTF-8
        d = coins; d.label_json = KilnStr{nullptr, 4}; try_def(d);
        d = coins; d.number_format_json = KilnStr{"\xc3", 1}; try_def(d);  // a cut UTF-8 sequence
        d = coins; d.reserved0 = 1; try_def(d);  // the named padding after sync_policy
        d = coins; d.reserved1 = 1; try_def(d);
        d = coins; d.reserved2 = 0x80000000u; try_def(d);
        check(bad == n && g_stub.calls == 0,
              "record size, enum values, flag bits, Currency+Carried, Carried without TRUST_IMPORTS, ids, read_mods "
              "outside READ_LIST, XP thresholds, labels, UTF-8 and a reserved field not 0: INVALID_ARGUMENT, the backend never asked");
        check(offsetof(KilnModValueDef, reserved0) == 36 && offsetof(KilnModValueDef, reserved1) == 124 &&
                  offsetof(KilnModValueDef, reserved2) == 140 && sizeof(KilnModValueDef) == 144,
              "KilnModValueDef's padding is named: reserved0 at 36, reserved1 at 124, reserved2 at 140 (144 bytes as before)");
        KilnModDefinitionResult r{sizeof r, 0, 0, 0};
        check(api->describe(fishing, nullptr, &r) == KILN_ERR_INVALID_ARGUMENT &&
                  api->describe(fishing, &coins, nullptr) == KILN_ERR_INVALID_ARGUMENT,
              "describe without a definition or a result: INVALID_ARGUMENT");
        r.size = 8;
        check(api->define(fishing, &coins, &r) == KILN_ERR_INVALID_ARGUMENT, "a result record below v1: INVALID_ARGUMENT");
        KilnModValueDef ok = def_of("lvl", KMV_EXPERIENCE);
        ok.xp_thresholds = t; ok.threshold_count = 3; ok.persistence = KMV_CARRIED; ok.flags = KMV_TRUST_IMPORTS | KMV_HAS_INITIAL;
        r = {sizeof r, 0, 0, 0};
        check(api->describe(fishing, &ok, &r) == KILN_OK && g_stub.calls == 1 && g_stub.def_kind == KMV_EXPERIENCE,
              "a valid carried experience definition with thresholds reaches the backend");
        KilnModValueDef longer[2] = {coins, coins};
        longer[0].size = sizeof longer;
        r = {sizeof r, 0, 0, 0};
        check(api->describe(fishing, longer, &r) == KILN_OK, "a definition record longer than v1 is accepted");
    }

    // ---- read: the one error rule and the flags ----
    {
        reset_stub();
        g_stub.ok = 0;
        g_stub.reason = KMV_UNDEFINED;
        g_stub.flags = KMV_BEFORE | KMV_AFTER;  // a careless backend: the core keeps the rule
        auto r = result_buf();
        check(api->read(fishing, &who, S("fishing:nope"), &r) == KILN_OK && r.ok == 0 && r.reason == KMV_UNDEFINED &&
                  r.flags == 0 && zero_value(r.before) && zero_value(r.after),
              "a refused read: KILN_OK, ok 0, reason UNDEFINED, flags 0, no snapshot");
        reset_stub();
        g_stub.flags = KMV_BEFORE | KMV_AFTER;
        r = result_buf();
        check(api->read(fishing, &who, S("fishing:coins"), &r) == KILN_OK && r.ok == 1 && r.reason == 0 &&
                  r.flags == KMV_AFTER && zero_value(r.before) && r.after.value == 11 && r.after.size == sizeof(KilnModValue) &&
                  r.after.world_rfc4122[0] == 0xab && std::memcmp(&r.character, &who, sizeof who) == 0 &&
                  g_stub.ns == "fishing" && g_stub.key == "fishing:coins",
              "a read: AFTER only, the caller's character, the namespace and the key passed on");
        reset_stub();
        g_stub.flags = 0;
        r = result_buf();
        check(api->read(fishing, &who, S("fishing:coins"), &r) == KILN_ERR_INTERNAL && payload_zero(r),
              "an accepted read without a value is the backend's fault: INTERNAL");
        reset_stub();
        g_stub.ok = 0;
        g_stub.reason = KMV_ACCESS_DENIED;
        r = result_buf();
        check(api->read(plugin, &who, S("fishing:coins"), &r) == KILN_OK && r.reason == KMV_ACCESS_DENIED &&
                  g_stub.ns == "native.979f28a72f2a8c18d48f73f0fe910c21",
              "reading another namespace's value is the backend's decision (a domain answer), with the reader's namespace");
        KilnModValueResult big[2];
        std::memset(big, 0x5a, sizeof big);
        big[0].size = sizeof big;
        reset_stub();
        check(api->read(fishing, &who, S("fishing:coins"), big) == KILN_OK && big[0].size == sizeof big &&
                  big[1].ok == 0x5a5a5a5a,
              "a larger result record: size kept, bytes past v1 untouched");
    }

    // ---- invalid reads and changes never reach the backend ----
    {
        reset_stub();
        auto r = result_buf();
        int bad = 0, n = 0;
        for (const char* key : {"", "fishing", "fishing:", ":coins", "Fishing:coins", "fishing:Coins", "fishing:1c",
                                "fishing:coins:x", "fishing coins", "fishing:co ins", "fishing::coins"}) {
            ++n;
            if (api->read(fishing, &who, S(key), &r) == KILN_ERR_INVALID_ARGUMENT) ++bad;
        }
        check(bad == n && g_stub.calls == 0, "qualified keys must be canonical ASCII namespace:id: INVALID_ARGUMENT");
        const KilnModCharacter none{};
        check(api->read(fishing, &none, S("fishing:coins"), &r) == KILN_ERR_INVALID_ARGUMENT,
              "account 0 with GUID zero is no character: INVALID_ARGUMENT");
        r.size = 100;
        check(api->read(fishing, &who, S("fishing:coins"), &r) == KILN_ERR_INVALID_ARGUMENT &&
                  api->read(fishing, &who, S("fishing:coins"), nullptr) == KILN_ERR_INVALID_ARGUMENT,
              "a result record below v1 or none: INVALID_ARGUMENT");
        r = result_buf();
        check(api->change(fishing, &who, S("fishing:coins"), 4, 1, 0, nullptr, 0, &r) == KILN_ERR_INVALID_ARGUMENT,
              "an unknown operation: INVALID_ARGUMENT");
        check(api->change(fishing, &who, S("fishing:coins"), KMV_ADD, 1, 2, nullptr, 0, &r) == KILN_ERR_INVALID_ARGUMENT,
              "an unknown request flag: INVALID_ARGUMENT");
        check(api->change(fishing, &who, S("fishing:coins"), KMV_ADD, 1, KMV_EXPECT_REVISION, nullptr, 0, &r) ==
                  KILN_ERR_INVALID_ARGUMENT,
              "EXPECT_REVISION without an expected version: INVALID_ARGUMENT");
        check(api->change(fishing, nullptr, S("fishing:coins"), KMV_ADD, 1, 0, nullptr, 0, &r) == KILN_ERR_INVALID_ARGUMENT,
              "change without a character: INVALID_ARGUMENT");
        check(g_stub.calls == 0, "none of them reached the backend");
    }

    // ---- change ----
    {
        reset_stub();
        g_stub.flags = KMV_BEFORE | KMV_AFTER;
        auto r = result_buf();
        KilnModValueVersion stale{};
        stale.revision = 77;
        check(api->change(fishing, &who, S("fishing:coins"), KMV_SPEND, 3, 0, &stale, 9001, &r) == KILN_OK && r.ok == 1 &&
                  r.flags == (KMV_BEFORE | KMV_AFTER) && r.before.value == 7 && r.after.value == 11 &&
                  g_stub.op == KMV_SPEND && g_stub.amount == 3 && g_stub.expected == nullptr && g_stub.request_id == 9001 &&
                  std::memcmp(&r.character, &who, sizeof who) == 0,
              "an accepted change: BEFORE|AFTER; without EXPECT_REVISION expected is ignored (NULL to the backend)");
        reset_stub();
        g_stub.flags = KMV_BEFORE | KMV_AFTER;
        KilnModValueVersion zero{};
        zero.world_rfc4122[0] = 0xab;
        r = result_buf();
        check(api->change(fishing, &who, S("fishing:coins"), KMV_SET, 50, KMV_EXPECT_REVISION, &zero, 0, &r) == KILN_OK &&
                  g_stub.expected != nullptr && g_stub.expected_copy.revision == 0 && g_stub.expected_copy.world_rfc4122[0] == 0xab &&
                  g_stub.request_id == 0,
              "EXPECT_REVISION with revision 0 is valid: the world and revision reach the backend; request id 0 is fine");
        reset_stub();
        g_stub.flags = KMV_AFTER;
        r = result_buf();
        check(api->change(fishing, &who, S("fishing:coins"), KMV_ADD, 1, 0, nullptr, 0, &r) == KILN_ERR_INTERNAL && payload_zero(r),
              "an accepted change without Before is the backend's fault: INTERNAL");
        reset_stub();
        g_stub.ok = 0;
        g_stub.reason = KMV_INSUFFICIENT_FUNDS;
        g_stub.flags = KMV_BEFORE | KMV_AFTER;
        r = result_buf();
        check(api->change(fishing, &who, S("fishing:coins"), KMV_SPEND, 30, 0, nullptr, 0, &r) == KILN_OK && r.ok == 0 &&
                  r.reason == KMV_INSUFFICIENT_FUNDS && r.flags == 0 && zero_value(r.before) && zero_value(r.after),
              "a refused change: KILN_OK, ok 0, the reason, flags 0, no snapshot");
        reset_stub();
        g_stub.rc = KILN_ERR_INVALID_ARGUMENT;  // e.g. SPEND of a counter, refused by Kiln.ModValues' rules
        r = result_buf();
        check(api->change(fishing, &who, S("fishing:xp"), KMV_SPEND, 1, 0, nullptr, 0, &r) == KILN_ERR_INVALID_ARGUMENT &&
                  payload_zero(r),
              "the backend's INVALID_ARGUMENT (an amount the operation refuses) comes through");
    }

    // ---- read_synced ----
    {
        reset_stub();
        g_stub.flags = KMV_AFTER;
        auto r = result_buf();
        check(api->read_synced(plugin, S("fishing:coins"), &r) == KILN_OK && r.ok == 1 && r.flags == KMV_AFTER &&
                  r.character.account == 0x77 && g_stub.ns == "native.979f28a72f2a8c18d48f73f0fe910c21",
              "read_synced: AFTER, the backend's local character, the reader's namespace");
        reset_stub();
        g_stub.ok = 0;
        g_stub.reason = KMV_NOT_SYNCED;
        g_stub.flags = KMV_AFTER;
        r = result_buf();
        check(api->read_synced(fishing, S("fishing:coins"), &r) == KILN_OK && r.reason == KMV_NOT_SYNCED && r.flags == 0,
              "read_synced refused: NOT_SYNCED, flags 0");
        reset_stub();
        g_stub.rc = KILN_ERR_UNAVAILABLE;  // stats.sync is off on a dedicated server
        r = result_buf();
        check(api->read_synced(fishing, S("fishing:coins"), &r) == KILN_ERR_UNAVAILABLE, "the backend's UNAVAILABLE comes through");
    }

    // ---- subscriptions ----
    {
        const KilnOwner other = reg::owner_create("plugin:reader.dll");
        char other_ns[KMV_NAMESPACE_CAPACITY];
        uint32_t need = 0;
        api->owner_namespace(other, other_ns, sizeof other_ns, &need);
        Seen own, cross, synced, elsewhere, all_of;
        KilnHandle h_own = 0, h_cross = 0, h_synced = 0, h_else = 0, h_all = 0, h = 0;
        check(api->subscribe(fishing, KMV_CANONICAL_EVENTS, S(""), &on_change, &own, &h_own) == KILN_OK && h_own != 0,
              "an empty filter subscribes to the owner's own namespace");
        check(be->wanted() == KMV_WANT_CANONICAL, "wanted() has the canonical bit only");
        api->subscribe(other, KMV_CANONICAL_EVENTS, S("fishing:coins"), &on_change, &cross, &h_cross);
        api->subscribe(other, KMV_SYNCED_EVENTS, S("fishing:*"), &on_change, &synced, &h_synced);
        api->subscribe(other, KMV_CANONICAL_EVENTS, S("other:coins"), &on_change, &elsewhere, &h_else);
        api->subscribe(other, KMV_CANONICAL_EVENTS, S("fishing:*"), &on_change, &all_of, &h_all);
        check(be->wanted() == (KMV_WANT_CANONICAL | KMV_WANT_SYNCED), "a synced subscriber sets the synced bit");
        int bad = 0;
        for (const char* f : {"fishing", "fishing:", "fishing:**", "*:coins", "Fishing:coins", ":*", "fishing:coins:x"})
            if (api->subscribe(other, KMV_CANONICAL_EVENTS, S(f), &on_change, &own, &h) == KILN_ERR_INVALID_ARGUMENT) ++bad;
        check(bad == 7, "filter syntax: namespace:id, namespace:* or empty, else INVALID_ARGUMENT");
        check(api->subscribe(other, 2, S(""), &on_change, &own, &h) == KILN_ERR_INVALID_ARGUMENT &&
                  api->subscribe(other, KMV_CANONICAL_EVENTS, S(""), nullptr, &own, &h) == KILN_ERR_INVALID_ARGUMENT &&
                  api->subscribe(other, KMV_CANONICAL_EVENTS, S(""), &on_change, &own, nullptr) == KILN_ERR_INVALID_ARGUMENT,
              "an unknown scope, no callback, no handle pointer: INVALID_ARGUMENT");
        check(api->subscribe(987654, KMV_CANONICAL_EVENTS, S(""), &on_change, &own, &h) == KILN_ERR_REVOKED,
              "a revoked owner cannot subscribe");

        auto c = event("fishing:coins", KMV_CANONICAL_EVENTS);
        auto owner_only = exported(KMV_OWNER_ONLY, KMV_SYNC_OWNER_MOD);
        check(be->dispatch(&c, &owner_only) == KILN_OK && own.calls == 1 && own.key == "fishing:coins" &&
                  own.kind == KMV_CHANGED && own.scope == KMV_CANONICAL_EVENTS && own.after == 99 && own.request_id == 42 &&
                  own.flags == (KMV_BEFORE | KMV_AFTER) && cross.calls == 0 && all_of.calls == 0 && synced.calls == 0 &&
                  elsewhere.calls == 0,
              "owner-only value: the owning namespace hears it; another namespace's filter does not (no read access), "
              "nor the synced scope or another key");
        auto pub = exported(KMV_PUBLIC_READ, KMV_SYNC_OWNER_MOD);
        be->dispatch(&c, &pub);
        check(own.calls == 2 && cross.calls == 1 && all_of.calls == 1 && synced.calls == 0 && elsewhere.calls == 0,
              "public value: the exact and the namespace filter of another namespace hear it too");
        KilnStr list[] = {S("someone"), S(other_ns)};
        auto listed = exported(KMV_READ_LIST, KMV_SYNC_OWNER_MOD, list, 2);
        auto c2 = event("fishing:xp", KMV_CANONICAL_EVENTS, KMV_INITIALIZED);
        be->dispatch(&c2, &listed);
        check(own.calls == 3 && cross.calls == 1 && all_of.calls == 2 && all_of.key == "fishing:xp" && all_of.kind == KMV_INITIALIZED,
              "read list naming the reader: its namespace filter hears fishing:xp, its fishing:coins filter does not");
        auto unlisted = exported(KMV_READ_LIST, KMV_SYNC_OWNER_MOD, list, 1);
        be->dispatch(&c2, &unlisted);
        check(all_of.calls == 2 && own.calls == 4, "read list without the reader: only the owner hears it (rechecked per event)");

        auto s = event("fishing:coins", KMV_SYNCED_EVENTS, KMV_AVAILABILITY_CHANGED);
        be->dispatch(&s, &pub);
        check(synced.calls == 0 && own.calls == 4, "synced event of a public value with sync OWNER_MOD: not for another namespace");
        auto readers = exported(KMV_PUBLIC_READ, KMV_SYNC_READERS);
        be->dispatch(&s, &readers);
        check(synced.calls == 1 && synced.scope == KMV_SYNCED_EVENTS && synced.kind == KMV_AVAILABILITY_CHANGED &&
                  own.calls == 4 && cross.calls == 1,
              "synced event with sync READERS and a public read: the synced subscriber hears it, canonical ones do not");

        auto badc = c;
        badc.size = 100;
        check(be->dispatch(&badc, &pub) == KILN_ERR_INVALID_ARGUMENT && be->dispatch(&c, nullptr) == KILN_ERR_INVALID_ARGUMENT,
              "dispatch refuses a short record or no export");
        badc = c;
        badc.qualified_key = S("Fishing:coins");
        check(be->dispatch(&badc, &pub) == KILN_ERR_INVALID_ARGUMENT, "dispatch refuses a bad key");

        // unsubscribe
        check(api->unsubscribe(h_cross) == KILN_OK && api->unsubscribe(h_cross) == KILN_ERR_NOT_FOUND &&
                  api->unsubscribe(0) == KILN_ERR_INVALID_ARGUMENT,
              "unsubscribe: OK once, then NOT_FOUND; handle 0: INVALID_ARGUMENT");
        be->dispatch(&c, &pub);
        check(cross.calls == 1 && own.calls == 5, "an unsubscribed callback is not called again");

        // unsubscribing itself from inside its callback (the dispatching thread) does not wait for itself
        own.unsubscribe_self = h_own;
        own.api = api;
        be->dispatch(&c, &pub);
        check(own.calls == 6 && api->unsubscribe(h_own) == KILN_ERR_NOT_FOUND, "a callback can unsubscribe itself");
        be->dispatch(&c, &pub);
        check(own.calls == 6, "and is not called again");

        // a faulting callback is removed
        Seen faulty;
        faulty.do_throw = true;
        KilnHandle h_faulty = 0;
        api->subscribe(fishing, KMV_CANONICAL_EVENTS, S(""), &on_change, &faulty, &h_faulty);
        be->dispatch(&c, &pub);
        be->dispatch(&c, &pub);
        check(faulty.calls == 1 && api->unsubscribe(h_faulty) == KILN_ERR_NOT_FOUND && logged("faulted"),
              "a callback that faults is removed after its first fault (logged)");

        // unsubscribe from another thread waits for a call in flight
        struct Slow {
            std::atomic<bool> started{false}, finished{false};
        } slow;
        KilnHandle h_slow = 0;
        api->subscribe(fishing, KMV_CANONICAL_EVENTS, S("fishing:slow"), [](const KilnModValueChange*, void* u) {
            auto* sl = static_cast<Slow*>(u);
            sl->started = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            sl->finished = true;
        }, &slow, &h_slow);
        auto cs = event("fishing:slow", KMV_CANONICAL_EVENTS);
        std::thread t([&] { be->dispatch(&cs, &owner_only); });
        while (!slow.started) std::this_thread::yield();
        const bool ok = api->unsubscribe(h_slow) == KILN_OK;
        const bool done_first = slow.finished.load();
        t.join();
        check(ok && done_first, "unsubscribe on another thread returns only after the call in flight has returned");

        api->unsubscribe(h_synced);
        check(be->wanted() == KMV_WANT_CANONICAL, "the last synced subscriber gone: the synced bit clears");
        api->unsubscribe(h_else);
        api->unsubscribe(h_all);
        check(be->wanted() == 0, "no subscriber left: wanted() is 0");
    }

    // ---- owner revoke ----
    {
        reset_stub();
        g_stub.retired.clear();
        const KilnOwner native = reg::owner_create("native:same");
        const KilnOwner twin = reg::owner_create("native:same");  // a second load of the same native mod
        const KilnOwner lurker = reg::owner_create("plugin:lurker.dll");
        KilnModDefinitionResult d{sizeof d, 0, 0, 0};
        api->describe(native, &coins, &d);
        api->define(twin, &coins, &d);
        Seen heard, lurk;
        KilnHandle h_native = 0, h_lurk = 0;
        api->subscribe(native, KMV_CANONICAL_EVENTS, S(""), &on_change, &heard, &h_native);
        api->subscribe(lurker, KMV_CANONICAL_EVENTS, S(""), &on_change, &lurk, &h_lurk);
        const std::string ns = "native.79231b3a6fa23ad352e1bb4e43bcf7cf";
        check(g_stub.owner == twin, "define passes the defining owner's handle");
        reg::owner_revoke(native);
        const std::string coins_key = ns + ":coins";  // the event borrows the key's bytes
        auto c = event(coins_key.c_str(), KMV_CANONICAL_EVENTS);
        auto pub = exported(KMV_PUBLIC_READ, KMV_SYNC_OWNER_MOD);
        be->dispatch(&c, &pub);
        check(heard.calls == 0 && api->unsubscribe(h_native) == KILN_ERR_NOT_FOUND,
              "revoking an owner removes its subscriptions");
        check(g_stub.retired.size() == 1 && g_stub.retired[0].first == ns && g_stub.retired[0].second == native,
              "revoking an owner that described retires exactly what it made: the backend gets its namespace and its owner "
              "handle as the token (the twin's definitions in the same namespace stay)");
        auto r = result_buf();
        check(api->read(native, &who, S("fishing:coins"), &r) == KILN_ERR_REVOKED && payload_zero(r) &&
                  api->subscribe(native, KMV_CANONICAL_EVENTS, S(""), &on_change, &heard, &h_native) == KILN_ERR_REVOKED,
              "a revoked owner's calls are REVOKED");
        reg::owner_revoke(twin);
        check(g_stub.retired.size() == 2 && g_stub.retired[1].first == ns && g_stub.retired[1].second == twin,
              "then the twin's revoke retires the twin's (data parked there)");
        reg::owner_revoke(lurker);
        check(g_stub.retired.size() == 2, "an owner that never described or defined retires nothing");
        reg::owner_revoke(spoof);
        reg::owner_revoke(fishing);
        check(g_stub.retired.size() == 3 && g_stub.retired[2].first == "fishing" && g_stub.retired[2].second == fishing,
              "a managed mod's owner retires in its manifest id's namespace with its own handle");
        check(be->wanted() == 0, "revoked subscribers leave wanted()");
    }

    std::printf("mod_values_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
