// SPDX-License-Identifier: MIT
// kiln.content, host test: a content transaction (store.cpp, store_txn_*) takes an owner's writes back whole.
// Compiles the real store.cpp over a made-up memory (a byte map): game-memory reads and the write batch are stubbed here, and the write batch
// can be made to fail at chosen addresses (fault injection at a write and at a restore). txn_test.sh builds and runs it.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include "../content.h"
#include "../store.cpp"

namespace kc {
const KilnApi* g_api = nullptr;
Engine E;
namespace {
std::map<uintptr_t, uint8_t> g_mem;     // the made-up game memory, byte by byte
std::set<uintptr_t> g_fail;             // writes to these addresses fail
std::set<uintptr_t> g_lie;              // writes to these addresses report success but leave memory as it was
Registry g_items;
}  // namespace
void logf(int, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fputc('\n', stderr);
}
bool rd(uintptr_t address, void* out, size_t n) {
    auto* o = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) {
        auto it = g_mem.find(address + i);
        if (it == g_mem.end()) return false;
        o[i] = it->second;
    }
    return true;
}
size_t write_batch(std::vector<Write>& writes, uintptr_t* failed) {
    size_t done = 0;
    for (const Write& w : writes) {
        if (g_fail.count(w.addr)) {
            if (failed && !*failed) *failed = w.addr;
            continue;
        }
        if (!g_lie.count(w.addr))
            for (uint8_t i = 0; i < w.n; ++i) g_mem[w.addr + i] = w.bytes[i];
        ++done;
    }
    return done;
}
Registry& reg(uint32_t) { return g_items; }
const Field* Schema::find(const std::string&) const { return nullptr; }
KilnContentValue decode(const Field&, const uint8_t*) { return {}; }
bool patch_code_byte(uintptr_t, uint8_t, uint8_t) { return false; }
}  // namespace kc

namespace {
using namespace kc;
int checks = 0;
void check(bool yes, const char* name) {
    if (!yes) throw std::runtime_error(name);
    ++checks;
}

constexpr KilnOwner A = 11, B = 22;
constexpr uintptr_t F1 = 0x1000, F2 = 0x1010, F3 = 0x1020, F4 = 0x1030;

uint32_t at(uintptr_t a) {
    uint32_t v = 0;
    rd(a, &v, 4);
    return v;
}
void mem(uintptr_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) g_mem[a + i] = static_cast<uint8_t>(v >> (8 * i));
}
bool put(KilnOwner o, uint64_t seq, uintptr_t a, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    return store_put(o, seq, 1, 1, 0, -1, a, 4, b, "test");
}
void flush() {
    std::string e;
    store_flush(&e);
}
void fresh() {
    std::string e;
    store_reset(false, &e);
    g_mem.clear();
    g_fail.clear();
    g_lie.clear();
    for (uintptr_t a : {F1, F2, F3, F4}) mem(a, 100);   // the game's values
}
TxnOutcome rollback(uint64_t id, KilnOwner* owner = nullptr, std::string* why = nullptr) {
    uint32_t n = 0;
    std::string w;
    const TxnOutcome o = store_txn_rollback(id, nullptr, owner, &n, &w);
    if (why) *why = w;
    return o;
}

// a rollback after writes that changed, added and removed layers puts back exactly the start: bytes, order against another owner, and memory
void restores_changed_added_and_removed_layers() {
    fresh();
    put(A, 5, F1, 1);
    put(B, 7, F1, 2);       // B's newer layer wins F1
    put(A, 9, F2, 3);
    flush();
    check(at(F1) == 2 && at(F2) == 3, "start: B wins F1, A holds F2");
    uint64_t id = 0;
    check(store_txn_begin(A, &id), "begin");
    check(!store_txn_begin(A, &id), "one open transaction per owner");
    check(store_txn_open(A) && !store_txn_open(B), "open for A only");
    put(A, 20, F1, 4);      // now A's is newer than B's: A wins F1
    store_drop(A, F2);      // F2 back to the game's
    put(A, 21, F3, 5);      // a field A never had
    flush();
    check(at(F1) == 4 && at(F2) == 100 && at(F3) == 5, "the writes took");
    std::string why;
    check(rollback(id, nullptr, &why) == TxnOutcome::Restored, ("restored: " + why).c_str());
    check(at(F1) == 2 && at(F2) == 3 && at(F3) == 100, "memory is the start's: B wins F1 again, A's F2 is back, F3 is the game's");
    check(!store_txn_open(A), "the transaction is closed");
    // the restored layer keeps its order: dropping B leaves A's old value on F1
    store_drop(B, F1);
    flush();
    check(at(F1) == 1, "A's restored layer is its old one, under B's");
}

// a commit keeps what was written; a second end of the same transaction finds nothing
void commit_keeps_the_writes() {
    fresh();
    uint64_t id = 0;
    check(store_txn_begin(A, &id), "begin");
    put(A, 1, F1, 7);
    flush();
    check(store_txn_commit(id), "commit");
    check(at(F1) == 7, "the write stays");
    check(!store_txn_commit(id) && rollback(id) == TxnOutcome::Cancelled, "ended once");
}

// fault injection at a write: the apply's flush fails on one field, the rollback puts the others back and reads every one back
void a_failed_write_is_taken_back() {
    fresh();
    put(A, 1, F1, 1);
    flush();
    uint64_t id = 0;
    check(store_txn_begin(A, &id), "begin");
    g_fail.insert(F2);      // the write of F2 fails, F1 and F3 take
    put(A, 2, F1, 8);
    put(A, 3, F2, 8);
    put(A, 4, F3, 8);
    std::string e;
    store_flush(&e);
    check(!e.empty() && at(F1) == 8 && at(F2) == 100 && at(F3) == 8, "half applied");
    g_fail.clear();
    check(rollback(id) == TxnOutcome::Restored, "restored");
    check(at(F1) == 1 && at(F2) == 100 && at(F3) == 100, "the start again");
}

// fault injection at a restore: a field whose put-back write fails, or that does not hold what was written, makes the rollback unverified
void a_failed_restore_is_unverified() {
    for (int lie = 0; lie < 2; ++lie) {
        fresh();
        put(A, 1, F1, 1);
        flush();
        uint64_t id = 0;
        check(store_txn_begin(A, &id), "begin");
        put(A, 2, F1, 9);
        put(A, 3, F4, 9);
        flush();
        (lie ? g_lie : g_fail).insert(F1);      // putting F1 back fails (or claims to and does not)
        KilnOwner owner = 0;
        std::string why;
        check(rollback(id, &owner, &why) == TxnOutcome::Unverified, "unverified");
        check(owner == A, "the owner is named");
        check(why.find("did not read back") != std::string::npos, "the reason says so");
        check(at(F4) == 100, "the other field was put back");
    }
}

// the owner's layers taken off during the transaction (revert_owner, the owner going, the world's reset): nothing is put back
void a_dropped_owner_cancels_the_transaction() {
    fresh();
    put(A, 1, F1, 1);
    flush();
    uint64_t id = 0;
    check(store_txn_begin(A, &id), "begin");
    put(A, 2, F2, 2);
    store_drop_owner(A);
    flush();
    check(rollback(id) == TxnOutcome::Cancelled, "cancelled");
    check(at(F1) == 100 && at(F2) == 100, "no layer of A came back");
    check(store_txn_begin(A, &id), "begin again");
    std::string e;
    store_reset(false, &e);
    check(rollback(id) == TxnOutcome::Cancelled, "the world's reset ends it too");
}

// a slot that went with the owner's last layer (no other owner held it) is made again on rollback, with the game's original
void a_slot_that_went_comes_back() {
    fresh();
    put(A, 1, F3, 42);
    flush();
    uint64_t id = 0;
    check(store_txn_begin(A, &id), "begin");
    store_drop(A, F3);
    flush();
    check(at(F3) == 100 && !store_has(F3), "the slot went");
    check(rollback(id) == TxnOutcome::Restored, "restored");
    check(at(F3) == 42 && store_has(F3), "the slot and A's value are back");
    store_drop(A, F3);
    flush();
    check(at(F3) == 100, "its original is the game's");
}

void a_new_layer_dropped_before_rollback_is_still_verified() {
    fresh();
    uint64_t id = 0;
    check(store_txn_begin(A, &id), "begin");
    put(A, 1, F1, 9);
    flush();
    g_fail.insert(F1);
    store_drop(A, F1);
    flush();
    check(at(F1) == 9, "reset failed, transaction bytes remain");
    check(rollback(id) == TxnOutcome::Unverified, "dropped transaction field must be read back");
}

void the_settle_hook_runs_before_the_read_back() {
    fresh();
    uint64_t id = 0;
    check(store_txn_begin(A, &id), "begin");
    put(A, 1, F1, 5);
    flush();
    bool ran = false;
    uint32_t n = 0;
    std::string why;
    KilnOwner o = 0;
    check(store_txn_rollback(id, [&] { ran = true; }, &o, &n, &why) == TxnOutcome::Restored && ran && n == 1, "settle ran, one field");
}
}  // namespace

int main() {
    try {
        restores_changed_added_and_removed_layers();
        commit_keeps_the_writes();
        a_failed_write_is_taken_back();
        a_failed_restore_is_unverified();
        a_dropped_owner_cancels_the_transaction();
        a_slot_that_went_comes_back();
        a_new_layer_dropped_before_rollback_is_still_verified();
        the_settle_hook_runs_before_the_read_back();
    } catch (const std::exception& e) {
        std::printf("txn-test: FAIL %s (after %d checks)\n", e.what(), checks);
        return 1;
    }
    std::printf("txn-test: %d checks passed\n", checks);
    return 0;
}
