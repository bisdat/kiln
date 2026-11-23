// SPDX-License-Identifier: MIT
// Host test of the owner-retiring observers of the owner registry (src/registry.cpp; kiln.hooks
// subscribe_owner_retiring, dev.28). Run by test-owner-retiring.sh (ASan/UBSan, TSan-free: the lock check is a
// try_lock from inside the callback). The fault guard and the log are stubs.
#include <windows.h>  // tests/stubs/windows.h

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/registry.cpp"

namespace {
int g_fail = 0, g_pass = 0;
std::vector<std::string> g_errors;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_pass : ++g_fail;
}
}  // namespace

namespace kiln {
namespace log {
void writef(int level, const char*, const char* fmt, ...) {
    if (level != KILN_LOG_ERROR) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_errors.push_back(buf);
}
}  // namespace log
namespace guard {
bool task(KilnTaskFn fn, void* user, unsigned long*) {
    fn(user);
    return true;
}
}  // namespace guard
namespace hooks::seh {
// The product guard is a structured-exception handler; here a C++ exception stands for the fault.
bool call_owner_retiring(void(KILN_CALL* fn)(KilnOwner, void*), KilnOwner retiring, void* user, unsigned long* code) {
    try {
        fn(retiring, user);
        return true;
    } catch (...) {
        *code = 0xC0000005;
        return false;
    }
}
}  // namespace hooks::seh
}  // namespace kiln

namespace reg = kiln::reg;

namespace {
struct Seen {
    std::vector<KilnOwner> owners;
    bool lock_free = true;      // the registry's locks were free inside every call
    bool owner_dead = true;     // the retiring owner was already dead inside every call
    std::thread::id thread;
    KilnHandle unsubscribe = 0; // unsubscribe this handle from inside the callback
    bool do_throw = false;
};
void KILN_CALL on_retiring(KilnOwner retiring, void* user) {
    auto* s = static_cast<Seen*>(user);
    s->owners.push_back(retiring);
    s->thread = std::this_thread::get_id();
    if (reg::g_mutex.try_lock()) reg::g_mutex.unlock();
    else s->lock_free = false;
    if (reg::g_task_mutex.try_lock()) reg::g_task_mutex.unlock();
    else s->lock_free = false;
    if (reg::owner_alive(retiring)) s->owner_dead = false;
    if (s->unsubscribe) reg::remove_owner_retiring(s->unsubscribe);
    if (s->do_throw) throw std::runtime_error("observer fault");
}
void KILN_CALL tick_fn(const KilnTickInfo*, void*) {}
void KILN_CALL task_fn(void*) {}

// An observer that waits, as kiln.camera's owner-retiring barrier does for callbacks still running on other threads
// (DECISIONS.md, "Camera owner-retiring barrier"): bounded, so a registry lock held across the call fails the test
// instead of hanging it.
std::atomic<bool> g_wait_entered{false}, g_other_done{false}, g_wait_timed_out{false};
void KILN_CALL waiting_observer(KilnOwner, void*) {
    g_wait_entered = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_other_done.load()) {
        if (std::chrono::steady_clock::now() > end) {
            g_wait_timed_out = true;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}  // namespace

int main() {
    const KilnOwner service = reg::owner_create("service:kiln-camera.dll");
    const KilnOwner mod_a = reg::owner_create("mod:a"), mod_b = reg::owner_create("mod:b");
    Seen seen;
    KilnHandle h = 0;
    check(reg::add_owner_retiring(service, &on_retiring, &seen, &h) == KILN_OK && h != 0, "an observer subscribes");
    check(reg::add_owner_retiring(service, nullptr, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT, "no callback is refused");
    check(reg::add_owner_retiring(9999, &on_retiring, &seen, nullptr) == KILN_ERR_REVOKED, "a dead observer owner is refused");

    KilnHandle tick = 0;
    reg::add_tick(mod_a, KILN_TICK_PRE, &tick_fn, nullptr, &tick);
    reg::owner_revoke(mod_a);
    check(seen.owners == std::vector<KilnOwner>{mod_a}, "a plain revoke calls the observer once with the retiring owner");
    check(seen.lock_free, "the call runs with no registry lock held");
    check(seen.owner_dead && reg::tick_sub_count(KILN_TICK_PRE) == 0,
          "the owner is already dead and its own subscriptions are gone when the observer runs");
    check(seen.thread == std::this_thread::get_id(), "the call runs on the revoking thread");
    reg::owner_revoke(mod_a);
    check(seen.owners.size() == 1, "revoking the same owner again tells nobody");
    reg::owner_revoke(0);
    reg::owner_revoke(424242);
    check(seen.owners.size() == 1, "owner 0 and an owner that never existed tell nobody");

    // the load-failure path: an owner created and revoked before it registered anything
    const KilnOwner failed = reg::owner_create("plugin:failed.dll");
    reg::owner_revoke(failed);
    check(seen.owners.size() == 2 && seen.owners.back() == failed, "an owner revoked right after its creation is reported too");

    // two observers; the first unsubscribes the second from inside its callback
    Seen second;
    KilnHandle h2 = 0;
    reg::add_owner_retiring(0, &on_retiring, &second, &h2);  // the core itself may observe
    seen.unsubscribe = h2;
    reg::owner_revoke(mod_b);
    check(seen.owners.back() == mod_b && second.owners.empty() && reg::owner_retiring_count() == 1,
          "an observer unsubscribed by an earlier callback of the same revoke is not called");
    seen.unsubscribe = 0;
    check(reg::remove_owner_retiring(h2) == KILN_ERR_NOT_FOUND, "its handle is gone");

    // an observer unsubscribes itself inside its callback
    Seen self;
    KilnHandle hs = 0;
    reg::add_owner_retiring(service, &on_retiring, &self, &hs);
    self.unsubscribe = hs;
    const KilnOwner mod_c = reg::owner_create("mod:c"), mod_d = reg::owner_create("mod:d");
    reg::owner_revoke(mod_c);
    reg::owner_revoke(mod_d);
    check(self.owners == std::vector<KilnOwner>{mod_c} && seen.owners.back() == mod_d,
          "unsubscribing inside the callback is safe and stops later calls");

    // a faulting observer is removed; the others still run
    Seen bad;
    bad.do_throw = true;
    KilnHandle hb = 0;
    reg::add_owner_retiring(service, &on_retiring, &bad, &hb);
    Seen after;
    reg::add_owner_retiring(service, &on_retiring, &after, nullptr);
    const KilnOwner mod_e = reg::owner_create("mod:e"), mod_f = reg::owner_create("mod:f");
    reg::owner_revoke(mod_e);
    reg::owner_revoke(mod_f);
    check(bad.owners.size() == 1 && after.owners.size() == 2 && g_errors.size() == 1 &&
              g_errors[0].find("faulted") != std::string::npos,
          "an observer that faults is removed and logged; later observers and later revokes go on");

    // the observer's own owner: its subscriptions go with it, and it is not told about itself
    const size_t before = seen.owners.size();
    reg::owner_revoke(service);
    check(seen.owners.size() == before && after.owners.size() == 2 && reg::owner_retiring_count() == 0,
          "an observer is not called for its own owner's revoke, and its subscriptions go with it");
    const KilnOwner mod_g = reg::owner_create("mod:g");
    reg::owner_revoke(mod_g);
    check(seen.owners.size() == before, "nothing is called after the observer's owner is gone");

    // a waiting observer on the revoking thread: another thread uses the registry meanwhile (owners, ticks, tasks),
    // which needs both registry locks, so neither is held across the observer
    {
        const KilnOwner waiter = reg::owner_create("service:waiter"), mod_w = reg::owner_create("mod:w");
        KilnHandle hw = 0;
        reg::add_owner_retiring(waiter, &waiting_observer, nullptr, &hw);
        std::thread revoker([&] { reg::owner_revoke(mod_w); });
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!g_wait_entered.load() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const KilnOwner fresh = reg::owner_create("mod:fresh");
        KilnHandle t = 0;
        const bool tick_ok = reg::add_tick(fresh, KILN_TICK_PRE, &tick_fn, nullptr, &t) == KILN_OK;
        const bool task_ok = reg::post(fresh, &task_fn, nullptr) == KILN_OK;
        const bool alive = reg::owner_alive(fresh) && !reg::owner_alive(mod_w);
        g_other_done = true;
        revoker.join();
        check(g_wait_entered.load() && !g_wait_timed_out.load() && tick_ok && task_ok && alive,
              "while an observer waits on the revoking thread, another thread creates owners, subscribes and posts");
        reg::remove_owner_retiring(hw);
        reg::owner_revoke(fresh);
        reg::owner_revoke(waiter);
    }

    // kiln.internal.owners: grants live with the owner and go when it is revoked
    {
        const KilnOwnersInternalApi* t = reg::owners_internal_table();
        check(t && t->header.size == 32 && t->header.version == 2 && t->grant && t->grants && t->owner_id,
              "kiln.internal.owners: the table is v2 (owner_id appended at 24) and complete");
        {  // v2 owner_id: the core's id of a live owner (kiln.registry binds its callers with it)
            const KilnOwner m = reg::owner_create("mod:Fishing-Plus");
            char buf[64];
            size_t len = 0;
            check(t->owner_id(m, buf, sizeof buf, &len) == KILN_OK && std::string(buf) == "mod:Fishing-Plus" && len == 16, "owner_id: a live owner's id");
            check(t->owner_id(m, buf, 4, &len) == KILN_ERR_BUFFER_TOO_SMALL && len == 16 && std::string(buf) == "mod", "a buffer too small: cut, NUL-terminated, the needed length");
            check(t->owner_id(m, nullptr, 0, &len) == KILN_ERR_BUFFER_TOO_SMALL && len == 16, "a size query");
            check(t->owner_id(0, buf, sizeof buf, &len) == KILN_ERR_INVALID_ARGUMENT && t->owner_id(m, nullptr, 8, &len) == KILN_ERR_INVALID_ARGUMENT,
                  "owner 0 and a NULL buffer with a capacity: invalid");
            check(t->owner_id(424244, buf, sizeof buf, &len) == KILN_ERR_REVOKED, "an owner never made: revoked");
            reg::owner_revoke(m);
            check(t->owner_id(m, buf, sizeof buf, &len) == KILN_ERR_REVOKED, "a revoked owner: revoked");
        }
        const KilnOwner mod_h = reg::owner_create("mod:h"), mod_i = reg::owner_create("mod:i");
        uint32_t bits = 99;
        check(t->grants(mod_h, &bits) == KILN_OK && bits == 0, "a new owner holds no grant");
        check(t->grant(mod_h, KILN_OWNER_GRANT_STEAM_MESSAGES) == KILN_OK && t->grants(mod_h, &bits) == KILN_OK &&
                  bits == KILN_OWNER_GRANT_STEAM_MESSAGES,
              "a granted owner holds the Steam messages grant");
        check(t->grant(mod_h, KILN_OWNER_GRANT_STEAM_MESSAGES) == KILN_OK && t->grants(mod_h, &bits) == KILN_OK &&
                  bits == KILN_OWNER_GRANT_STEAM_MESSAGES,
              "granting it again changes nothing");
        bits = 99;
        check(t->grants(mod_i, &bits) == KILN_OK && bits == 0, "another owner's grant is its own");
        check(t->grant(0, KILN_OWNER_GRANT_STEAM_MESSAGES) == KILN_ERR_INVALID_ARGUMENT &&
                  t->grant(mod_i, 0) == KILN_ERR_INVALID_ARGUMENT && t->grant(mod_i, 2) == KILN_ERR_INVALID_ARGUMENT &&
                  t->grants(0, &bits) == KILN_ERR_INVALID_ARGUMENT && t->grants(mod_i, nullptr) == KILN_ERR_INVALID_ARGUMENT,
              "owner 0, no bit, an unknown bit and no out pointer are refused");
        check(t->grant(424243, KILN_OWNER_GRANT_STEAM_MESSAGES) == KILN_ERR_REVOKED &&
                  t->grants(424243, &bits) == KILN_ERR_REVOKED,
              "an owner that never existed is not alive: neither granted nor read");
        reg::owner_revoke(mod_h);
        check(t->grants(mod_h, &bits) == KILN_ERR_REVOKED && t->grant(mod_h, KILN_OWNER_GRANT_STEAM_MESSAGES) == KILN_ERR_REVOKED,
              "a revoked owner's grant is gone, and it cannot be granted again");
        const KilnOwner mod_h2 = reg::owner_create("mod:h");
        check(t->grants(mod_h2, &bits) == KILN_OK && bits == 0, "a reloaded mod's new owner starts with no grant");
        reg::owner_revoke(mod_h2);
        reg::owner_revoke(mod_i);
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
