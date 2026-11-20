// SPDX-License-Identifier: MIT
// kiln.mod-values v1 (abi/services/kiln_mod_values.h) and the Kiln-internal kiln.internal.mod-values.backend v1
// (../mod_values_host.h): the native facade over the managed ledger of Kiln.ModValues (dossier chapter 05b section 5).
// The core owns what is native: owner namespaces, argument checks, the result rules (a refusal is KILN_OK with ok 0;
// Before/After flags), the subscriptions and owner revokes. Everything about values is the backend's: Kiln.ModValues
// binds its process-lifetime functions once, at its start (managed/services/Kiln.ModValues/NativeBackend.cs), through
// the backend table only the managed host's KilnApi resolves (api.cpp), and delivers its events through dispatch.
// Host-tested with a stub backend against the real owner registry: tests/mod_values_test.cpp.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../common.h"
#include "../guard.h"
#include "../log.h"
#include "../mod_values_host.h"
#include "../registry.h"
#include "service.h"
#include "services/kiln_capacity_util.hpp"  // the core's portable SHA-256
#include "services/kiln_mod_values.h"

namespace kiln::mod_values {
namespace {

// ---- syntax --------------------------------------------------------------------------------------------------------

bool ns_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; }

// ^[a-z0-9][a-z0-9._-]{0,63}$: a manifest id (Kiln.Host Manifest.IdPattern), and "native." + 32 hex digits
bool is_namespace(std::string_view s) {
    if (s.empty() || s.size() > KMV_MAX_ID_BYTES) return false;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return false;
    return std::all_of(s.begin(), s.end(), ns_char);
}

// ^[a-z][a-z0-9._-]{0,63}$ (Kiln.ModValues ModValueRules.IsId)
bool is_id(std::string_view s) { return !s.empty() && s[0] >= 'a' && s[0] <= 'z' && is_namespace(s); }

// (NULL, 0) or bytes that may be read
bool readable(KilnStr s) { return s.len == 0 || s.ptr != nullptr; }

// "namespace:id", canonical ASCII, exactly one colon (neither part may hold one)
bool parse_key(KilnStr s, std::string_view* ns, std::string_view* id) {
    if (!readable(s) || s.len == 0 || s.len > KMV_MAX_KEY_BYTES) return false;
    const std::string_view t(s.ptr, s.len);
    const size_t colon = t.find(':');
    if (colon == std::string_view::npos) return false;
    *ns = t.substr(0, colon);
    *id = t.substr(colon + 1);
    return is_namespace(*ns) && is_id(*id);
}

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF, no cut sequence.
bool utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t n;
        uint32_t cp;
        if ((c & 0xe0) == 0xc0) n = 1, cp = c & 0x1fu;
        else if ((c & 0xf0) == 0xe0) n = 2, cp = c & 0x0fu;
        else if ((c & 0xf8) == 0xf0) n = 3, cp = c & 0x07u;
        else return false;
        if (s.size() - i <= n) return false;
        for (size_t k = 1; k <= n; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3fu);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += n + 1;
    }
    return true;
}

bool check_json(KilnStr s, bool required) {
    if (!readable(s)) return false;
    if (s.len == 0) return !required;
    return s.len <= KMV_MAX_JSON_BYTES && utf8(std::string_view(s.ptr, s.len));
}

constexpr uint32_t kDefFlags = KMV_TRUST_IMPORTS | KMV_ALLOW_PARTITION_MOVE | KMV_RESET_KIND_CHANGE | KMV_HAS_INITIAL;

// The definition's shape (chapter 05b section 5: these are invalid arguments). Bounds, scale, versions, labels'
// languages and number formats are Kiln.ModValues' (its ArgumentException comes back as INVALID_ARGUMENT).
bool check_def(const KilnModValueDef* d) {
    if (!d || d->size < sizeof(KilnModValueDef)) return false;
    if (d->kind > KMV_EXPERIENCE || d->persistence > KMV_CARRIED || d->read_policy > KMV_READ_LIST ||
        d->sync_policy > KMV_SYNC_READERS || (d->flags & ~kDefFlags) != 0)
        return false;
    if (d->reserved0 != 0 || d->reserved1 != 0 || d->reserved2 != 0) return false;  // free for a later version
    if (!readable(d->local_id) || !is_id(sv(d->local_id))) return false;
    if (!check_json(d->label_json, true) || !check_json(d->number_format_json, false)) return false;
    if (d->persistence == KMV_CARRIED && (d->kind == KMV_CURRENCY || (d->flags & KMV_TRUST_IMPORTS) == 0)) return false;
    if (d->threshold_count > KMV_MAX_THRESHOLDS || (d->threshold_count != 0 && !d->xp_thresholds)) return false;
    if (d->kind == KMV_EXPERIENCE) {
        if (d->threshold_count == 0 || d->xp_thresholds[0] != 0) return false;
        for (uint32_t k = 1; k < d->threshold_count; ++k)
            if (d->xp_thresholds[k] <= d->xp_thresholds[k - 1]) return false;
    } else if (d->threshold_count != 0) {
        return false;
    }
    if (d->read_mod_count > KMV_MAX_READ_MODS || (d->read_mod_count != 0 && !d->read_mods)) return false;
    if (d->read_mod_count != 0 && d->read_policy != KMV_READ_LIST) return false;
    for (uint32_t k = 0; k < d->read_mod_count; ++k)
        if (!readable(d->read_mods[k]) || !is_namespace(sv(d->read_mods[k]))) return false;
    return true;
}

// Account 0 with GUID zero is no character (an account-only key, GUID zero with an account, is a domain answer).
bool check_character(const KilnModCharacter* c) {
    if (!c) return false;
    if (c->account != 0) return true;
    return std::any_of(std::begin(c->guid_rfc4122), std::end(c->guid_rfc4122), [](uint8_t b) { return b != 0; });
}

// ---- owners and their namespaces -------------------------------------------------------------------------------------

constexpr std::string_view kNativePrefix = "native.";

// A managed mod's owner is one Kiln.Host made through its own KilnApi (reg::owner_managed_host) and named
// "mod:<manifest id>" (Kiln.Host Mods.LoadCore): the id is its namespace, as Kiln.ModValues gives the mod. Every other
// owner, a native one named "mod:<id>" too: "native." + the first 32 hex digits of the SHA-256 of its exact name.
// "native." stays the hashed namespaces' own: Kiln.Host refuses manifest ids that start with it, and here a host
// owner "mod:native.<x>" is hashed as well.
std::string namespace_for(std::string_view owner_name, bool managed_host) {
    if (managed_host && owner_name.size() > 4 && owner_name.substr(0, 4) == "mod:" && is_namespace(owner_name.substr(4)) &&
        !owner_name.substr(4).starts_with(kNativePrefix))
        return std::string(owner_name.substr(4));
    const auto h = capacity::detail::sha256(reinterpret_cast<const uint8_t*>(owner_name.data()), owner_name.size());
    std::string ns(kNativePrefix);
    capacity::detail::hex(ns, h.data(), 16);
    return ns;
}

struct OwnerState {
    std::string ns;
    bool defines = false;  // described or defined through this facade: what it made is retired with it
};

struct Sub {
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    uint32_t scope = 0;
    std::string ns;         // the subscriber's namespace (what it may read)
    std::string filter_ns;  // the namespace its filter names
    std::string filter_id;  // "" = every value of filter_ns
    KilnModValueChangedFn fn = nullptr;
    void* user = nullptr;
    bool live = true;    // under g_mutex
    uint32_t calls = 0;  // calls in flight, under g_mutex
};

std::mutex g_mutex;               // g_owners, g_subs, Sub::live/calls; may call reg::owner_alive (never the reverse)
std::condition_variable g_idle;   // a subscriber's call ended
std::unordered_map<KilnOwner, OwnerState> g_owners;
std::vector<std::shared_ptr<Sub>> g_subs;
KilnHandle g_next_handle = 1;
std::atomic<uint32_t> g_wanted{0};

std::mutex g_bind_mutex;
KilnModValuesBackend g_backend_copy{};
std::atomic<const KilnModValuesBackend*> g_backend{nullptr};
KilnHandle g_retiring = 0;
std::atomic<bool> g_running{false};  // init ran: the revoke observer is in place, the managed host may bind

// The subscriptions this thread is calling right now (dispatch inside a callback nests): an unsubscribe or a revoke
// from inside a callback does not wait for the calls of its own thread. Trivial types only: no TLS destructor.
constexpr int kStack = 16;
thread_local const Sub* t_stack[kStack];
thread_local int t_depth = 0;

const KilnModValuesBackend* backend() { return g_backend.load(std::memory_order_acquire); }

// The owner's namespace. REVOKED for a revoked owner, or one that never existed.
KilnResult owner_ns(KilnOwner owner, std::string* out) {
    if (!reg::owner_alive(owner)) return KILN_ERR_REVOKED;
    {
        std::lock_guard lock(g_mutex);
        if (auto it = g_owners.find(owner); it != g_owners.end()) {
            *out = it->second.ns;
            return KILN_OK;
        }
    }
    const std::string name = reg::owner_name(owner);
    const bool host = reg::owner_managed_host(owner);  // fixed for the owner's life (owner ids are never reused)
    if (!reg::owner_alive(owner)) return KILN_ERR_REVOKED;  // the name read may be a revoked owner's placeholder
    *out = namespace_for(name, host);
    std::lock_guard lock(g_mutex);
    g_owners.try_emplace(owner, OwnerState{*out, false});
    return KILN_OK;
}

void update_wanted_locked() {
    uint32_t bits = 0;
    for (const auto& s : g_subs) bits |= 1u << s->scope;
    g_wanted.store(bits, std::memory_order_release);
}

// Waits until no call of s is in flight on another thread (s is no longer live, so none starts).
void wait_idle_locked(std::unique_lock<std::mutex>& lock, const Sub* s) {
    if (t_depth > kStack) return;  // nested deeper than recorded: do not risk waiting for this very thread
    uint32_t mine = 0;
    for (int i = 0; i < t_depth; ++i)
        if (t_stack[i] == s) ++mine;
    g_idle.wait(lock, [&] { return s->calls <= mine; });
}

// An owner is gone (revoked): its subscriptions are removed (no call of them is running when this returns, except on
// this thread), and when it described or defined, the backend retires what it made: the definitions registered with its
// handle as the token leave the described and active views (another owner's in the same namespace stay; the data is
// parked).
void owner_gone(KilnOwner owner) {
    std::string retire;
    {
        std::unique_lock lock(g_mutex);
        std::vector<std::shared_ptr<Sub>> removed;
        std::erase_if(g_subs, [&](const std::shared_ptr<Sub>& s) {
            if (s->owner != owner) return false;
            s->live = false;
            removed.push_back(s);
            return true;
        });
        if (!removed.empty()) update_wanted_locked();
        if (auto it = g_owners.find(owner); it != g_owners.end()) {
            if (it->second.defines) retire = std::move(it->second.ns);
            g_owners.erase(it);
        }
        for (const auto& s : removed) wait_idle_locked(lock, s.get());
    }
    if (retire.empty()) return;
    if (const auto* b = backend()) {
        KLOG_I("kiln.mod-values: owner %llu was revoked: the definitions it made in %s are retired (data kept)",
               static_cast<unsigned long long>(owner), retire.c_str());
        b->retire(kstr(retire), owner);
    }
}

void KILN_CALL on_owner_retiring(KilnOwner owner, void*) { owner_gone(owner); }

// The owner described or defined in ns: retire ns with it. A revoke that raced past it is caught up here.
void mark_defines(KilnOwner owner, const std::string& ns) {
    {
        std::lock_guard lock(g_mutex);
        auto& st = g_owners[owner];
        st.ns = ns;
        st.defines = true;
    }
    if (!reg::owner_alive(owner)) owner_gone(owner);
}

// ---- results -------------------------------------------------------------------------------------------------------

KilnModValueResult fresh_result() {
    KilnModValueResult r{};
    r.size = sizeof r;
    r.before.size = r.after.size = sizeof(KilnModValue);
    return r;
}

void zero(KilnModValueResult* out) {
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->before.size = out->after.size = sizeof(KilnModValue);
}

void zero(KilnModDefinitionResult* out) {
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
}

enum class Answer { Read, Change };

// The backend's answer under the chapter's rules: refused -> flags 0, no snapshot; read -> AFTER; accepted change ->
// BEFORE|AFTER. An answer that breaks them is the backend's fault (INTERNAL, logged). Copies v1 into out.
KilnResult finish(KilnResult rc, KilnModValueResult& r, Answer a, const KilnModCharacter* character,
                  KilnModValueResult* out, const char* what) {
    if (rc == KILN_OK) {
        if (r.ok == 0) {
            if (r.reason == KMV_NONE) {
                KLOG_E("kiln.mod-values: %s: the backend refused without a reason", what);
                rc = KILN_ERR_INTERNAL;
            } else {
                r.flags = 0;
                r.before = KilnModValue{};
                r.after = KilnModValue{};
            }
        } else {
            const uint32_t need = a == Answer::Read ? uint32_t{KMV_AFTER} : uint32_t{KMV_BEFORE | KMV_AFTER};
            if ((r.flags & need) != need) {
                KLOG_E("kiln.mod-values: %s: the backend accepted without its snapshot (flags %u)", what, r.flags);
                rc = KILN_ERR_INTERNAL;
            } else {
                r.ok = 1;
                r.reason = KMV_NONE;
                r.flags = need;
                if (a == Answer::Read) r.before = KilnModValue{};
            }
        }
    }
    if (rc != KILN_OK) {
        zero(out);
        return rc;
    }
    if (character) r.character = *character;
    r.before.size = r.after.size = sizeof(KilnModValue);
    const uint32_t size = out->size;
    std::memcpy(out, &r, sizeof r);
    out->size = size;
    return KILN_OK;
}

// ---- kiln.mod-values -----------------------------------------------------------------------------------------------

KilnResult KILN_CALL mv_owner_namespace(KilnOwner owner, char* utf8_out, uint32_t capacity, uint32_t* required) {
    if (!required || (!utf8_out && capacity != 0)) return KILN_ERR_INVALID_ARGUMENT;
    std::string ns;
    if (const KilnResult rc = owner_ns(owner, &ns); rc != KILN_OK) return rc;
    const auto need = static_cast<uint32_t>(ns.size() + 1);
    *required = need;
    if (capacity < need) return KILN_ERR_BUFFER_TOO_SMALL;  // never a truncated namespace
    std::memcpy(utf8_out, ns.data(), ns.size());
    utf8_out[ns.size()] = '\0';
    return KILN_OK;
}

KilnResult definition(bool activate, KilnOwner owner, const KilnModValueDef* def, KilnModDefinitionResult* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    if (!check_def(def)) {
        zero(out);
        return KILN_ERR_INVALID_ARGUMENT;
    }
    std::string ns;
    if (const KilnResult rc = owner_ns(owner, &ns); rc != KILN_OK) {
        zero(out);
        return rc;
    }
    const auto* b = backend();
    if (!b) {
        zero(out);
        return KILN_ERR_UNAVAILABLE;
    }
    KilnModValueDef v1 = *def;  // the backend reads v1 exactly
    v1.size = sizeof v1;
    KilnModDefinitionResult r{};
    r.size = sizeof r;
    KilnResult rc = (activate ? b->define : b->describe)(kstr(ns), owner, &v1, &r);
    if (rc == KILN_OK) {
        if (r.ok != 0) {
            r.ok = 1;
            r.reason = KMV_NONE;
            mark_defines(owner, ns);
        } else if (r.reason == KMV_NONE) {
            KLOG_E("kiln.mod-values: %s: the backend refused without a reason", activate ? "define" : "describe");
            rc = KILN_ERR_INTERNAL;
        }
    }
    if (rc != KILN_OK) {
        zero(out);
        return rc;
    }
    out->ok = r.ok;
    out->reason = r.reason;
    out->reserved = 0;
    return KILN_OK;
}

KilnResult KILN_CALL mv_describe(KilnOwner owner, const KilnModValueDef* def, KilnModDefinitionResult* out) {
    return definition(false, owner, def, out);
}

KilnResult KILN_CALL mv_define(KilnOwner owner, const KilnModValueDef* def, KilnModDefinitionResult* out) {
    return definition(true, owner, def, out);
}

// The common start of read, change and read_synced: out's record, the arguments (ok), the owner, the backend.
KilnResult start_call(KilnOwner owner, bool args_ok, KilnModValueResult* out, std::string* ns,
                 const KilnModValuesBackend** b) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    KilnResult rc = KILN_OK;
    if (!args_ok) rc = KILN_ERR_INVALID_ARGUMENT;
    else if ((rc = owner_ns(owner, ns)) == KILN_OK && !(*b = backend())) rc = KILN_ERR_UNAVAILABLE;
    if (rc != KILN_OK) zero(out);
    return rc;
}

KilnResult KILN_CALL mv_read(KilnOwner owner, const KilnModCharacter* character, KilnStr key, KilnModValueResult* out) {
    std::string_view kns, kid;
    std::string ns;
    const KilnModValuesBackend* b = nullptr;
    const bool args = check_character(character) && parse_key(key, &kns, &kid);
    if (const KilnResult rc = start_call(owner, args, out, &ns, &b); rc != KILN_OK) return rc;
    const KilnModCharacter who = *character;
    KilnModValueResult r = fresh_result();
    const KilnResult rc = b->read(kstr(ns), &who, key, &r);
    return finish(rc, r, Answer::Read, &who, out, "read");
}

KilnResult KILN_CALL mv_change(KilnOwner owner, const KilnModCharacter* character, KilnStr key, uint32_t operation,
                               int64_t amount, uint32_t request_flags, const KilnModValueVersion* expected,
                               uint64_t request_id, KilnModValueResult* out) {
    std::string_view kns, kid;
    std::string ns;
    const KilnModValuesBackend* b = nullptr;
    const bool expect = (request_flags & KMV_EXPECT_REVISION) != 0;
    const bool args = check_character(character) && parse_key(key, &kns, &kid) && operation <= KMV_RESET &&
                      (request_flags & ~uint32_t{KMV_EXPECT_REVISION}) == 0 && (!expect || expected);
    if (const KilnResult rc = start_call(owner, args, out, &ns, &b); rc != KILN_OK) return rc;
    const KilnModCharacter who = *character;
    KilnModValueVersion want{};
    if (expect) want = *expected;
    KilnModValueResult r = fresh_result();
    const KilnResult rc = b->change(kstr(ns), &who, key, operation, amount, expect ? &want : nullptr, request_id, &r);
    return finish(rc, r, Answer::Change, &who, out, "change");
}

KilnResult KILN_CALL mv_read_synced(KilnOwner owner, KilnStr key, KilnModValueResult* out) {
    std::string_view kns, kid;
    std::string ns;
    const KilnModValuesBackend* b = nullptr;
    if (const KilnResult rc = start_call(owner, parse_key(key, &kns, &kid), out, &ns, &b); rc != KILN_OK) return rc;
    KilnModValueResult r = fresh_result();
    const KilnResult rc = b->read_synced(kstr(ns), key, &r);
    return finish(rc, r, Answer::Read, nullptr, out, "read_synced");
}

KilnResult KILN_CALL mv_subscribe(KilnOwner owner, uint32_t scope, KilnStr filter, KilnModValueChangedFn fn, void* user,
                                  KilnHandle* out) {
    if (scope > KMV_SYNCED_EVENTS || !fn || !out || !readable(filter)) return KILN_ERR_INVALID_ARGUMENT;
    std::string_view fns, fid;
    if (filter.len != 0) {
        const std::string_view t(filter.ptr, filter.len);
        const size_t colon = t.find(':');
        if (colon == std::string_view::npos) return KILN_ERR_INVALID_ARGUMENT;
        fns = t.substr(0, colon);
        fid = t.substr(colon + 1);
        if (!is_namespace(fns) || !(fid == "*" || is_id(fid))) return KILN_ERR_INVALID_ARGUMENT;
        if (fid == "*") fid = {};
    }
    std::string ns;
    if (const KilnResult rc = owner_ns(owner, &ns); rc != KILN_OK) return rc;
    auto s = std::make_shared<Sub>();
    s->owner = owner;
    s->scope = scope;
    s->ns = ns;
    s->filter_ns = filter.len != 0 ? std::string(fns) : ns;
    s->filter_id = std::string(fid);
    s->fn = fn;
    s->user = user;
    {
        std::lock_guard lock(g_mutex);
        // checked under the lock: the revoke observer runs after the owner is dead and takes this lock, so it either
        // already ran (refused here) or runs after and removes this subscription
        if (!reg::owner_alive(owner)) return KILN_ERR_REVOKED;
        s->handle = g_next_handle++;
        g_subs.push_back(s);
        update_wanted_locked();
    }
    *out = s->handle;
    return KILN_OK;
}

KilnResult KILN_CALL mv_unsubscribe(KilnHandle handle) {
    if (handle == 0) return KILN_ERR_INVALID_ARGUMENT;
    std::unique_lock lock(g_mutex);
    const auto it = std::find_if(g_subs.begin(), g_subs.end(), [&](const auto& s) { return s->handle == handle; });
    if (it == g_subs.end()) return KILN_ERR_NOT_FOUND;
    const std::shared_ptr<Sub> s = *it;
    s->live = false;
    g_subs.erase(it);
    update_wanted_locked();
    wait_idle_locked(lock, s.get());
    return KILN_OK;
}

const KilnModValuesApi kTable = {{sizeof(KilnModValuesApi), KILN_MOD_VALUES_VERSION},
                                 &mv_owner_namespace,
                                 &mv_describe,
                                 &mv_define,
                                 &mv_read,
                                 &mv_change,
                                 &mv_read_synced,
                                 &mv_subscribe,
                                 &mv_unsubscribe};

// ---- kiln.internal.mod-values.backend ------------------------------------------------------------------------------

KilnResult KILN_CALL be_bind(const KilnModValuesBackend* b) {
    // exactly this build's record: a Kiln-internal table is not append-only
    if (!b || b->size != sizeof *b || !b->describe || !b->define || !b->read || !b->change || !b->read_synced || !b->retire)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_bind_mutex);
    if (g_backend.load(std::memory_order_acquire)) {
        KLOG_W("kiln.mod-values: a backend is bound already; a second bind is refused");
        return KILN_ERR_INVALID_ARGUMENT;
    }
    g_backend_copy = *b;
    g_backend_copy.size = sizeof g_backend_copy;
    g_backend.store(&g_backend_copy, std::memory_order_release);
    KLOG_I("kiln.mod-values: Kiln.ModValues' backend is bound: native callers reach the ledger");
    return KILN_OK;
}

uint32_t KILN_CALL be_wanted() { return g_wanted.load(std::memory_order_acquire); }

// May reader (a namespace) hear an event of a value of ns? Its own namespace always; another one by the value's read
// policy now, and for the synced copy only when the value is synced for readers (Kiln.ModValues ModValuesClient.Read).
bool may_read(const std::string& reader, std::string_view ns, uint32_t scope, const KilnModValueExport& x) {
    if (reader == ns) return true;
    if (scope == KMV_SYNCED_EVENTS && x.sync_policy != KMV_SYNC_READERS) return false;
    if (x.read_policy == KMV_PUBLIC_READ) return true;
    if (x.read_policy != KMV_READ_LIST) return false;
    for (uint32_t k = 0; k < x.read_mod_count; ++k)
        if (sv(x.read_mods[k]) == reader) return true;
    return false;
}

struct Call {
    KilnModValueChangedFn fn;
    const KilnModValueChange* change;
    void* user;
};

void KILN_CALL run_call(void* user) {
    const auto* c = static_cast<const Call*>(user);
    c->fn(c->change, c->user);
}

KilnResult KILN_CALL be_dispatch(const KilnModValueChange* change, const KilnModValueExport* x) {
    if (!change || change->size < sizeof *change || change->event_scope > KMV_SYNCED_EVENTS || !x || x->size < sizeof *x ||
        x->read_mod_count > KMV_MAX_READ_MODS || (x->read_mod_count != 0 && !x->read_mods))
        return KILN_ERR_INVALID_ARGUMENT;
    std::string_view ns, id;
    if (!parse_key(change->qualified_key, &ns, &id)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t scope = change->event_scope;
    if ((g_wanted.load(std::memory_order_acquire) & (1u << scope)) == 0) return KILN_OK;
    KilnModValueChange record = *change;  // v1, Kiln's sizes
    record.size = sizeof record;
    record.result.size = sizeof record.result;
    record.result.before.size = record.result.after.size = sizeof(KilnModValue);
    std::vector<std::shared_ptr<Sub>> targets;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& s : g_subs)
            if (s->live && s->scope == scope && s->filter_ns == ns && (s->filter_id.empty() || s->filter_id == id) &&
                may_read(s->ns, ns, scope, *x))
                targets.push_back(s);
    }
    for (const auto& s : targets) {
        {
            std::lock_guard lock(g_mutex);
            if (!s->live) continue;  // unsubscribed or revoked by an earlier callback of this event
            ++s->calls;
        }
        Call call{s->fn, &record, s->user};
        unsigned long code = 0;
        if (t_depth < kStack) t_stack[t_depth] = s.get();
        ++t_depth;
        const bool ok = guard::task(&run_call, &call, &code);
        --t_depth;
        {
            std::lock_guard lock(g_mutex);
            --s->calls;
            if (!ok && s->live) {
                s->live = false;
                std::erase(g_subs, s);
                update_wanted_locked();
            }
        }
        g_idle.notify_all();
        if (!ok)
            KLOG_E("kiln.mod-values: a subscriber of %s faulted (0x%08lx) and was removed", reg::owner_name(s->owner).c_str(),
                   code);
    }
    return KILN_OK;
}

const KilnModValuesBackendApi kBackendTable = {{sizeof(KilnModValuesBackendApi), KILN_MOD_VALUES_BACKEND_VERSION},
                                               &be_bind,
                                               &be_wanted,
                                               &be_dispatch};

bool init() {
    if (reg::add_owner_retiring(0, &on_owner_retiring, nullptr, &g_retiring) != KILN_OK) {
        KLOG_E("kiln.mod-values: no owner-retiring observer; the service is unavailable");
        return false;
    }
    g_running.store(true, std::memory_order_release);
    return true;
}

}  // namespace

// kiln.internal.mod-values.backend v1 for the managed host's KilnApi only (api.cpp api_service_managed_host): no in-core
// service, so no other view's lookup finds it. NULL until kiln.mod-values' init ran (and when it failed).
const void* backend_table() { return g_running.load(std::memory_order_acquire) ? &kBackendTable : nullptr; }

KILN_SERVICE(mod_values, KILN_MOD_VALUES_SERVICE, KILN_MOD_VALUES_VERSION, &kTable, &init);

}  // namespace kiln::mod_values
