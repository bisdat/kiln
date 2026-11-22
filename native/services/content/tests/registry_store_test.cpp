// SPDX-License-Identifier: MIT
// kiln.content, host test: registry layers in the store (registry P1 slice S9): the seq band above every data-pack layer and
// below code, evaluated (relative) layers computed over the value below them at every flush, and compare-before-write (sfi R3):
// a slot another writer wrote after Kiln's layer is never silently overwritten by a registry write or revert, it is skipped
// and reported, and stays with that writer for the rest of the world. Compiles the real store.cpp over a made-up memory, as
// txn_test.cpp does. registry_store_test.sh builds and runs it.
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
std::map<uintptr_t, uint8_t> g_mem;  // the made-up game memory, byte by byte
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
size_t write_batch(std::vector<Write>& writes, uintptr_t*) {
    for (const Write& w : writes)
        for (uint8_t i = 0; i < w.n; ++i) g_mem[w.addr + i] = w.bytes[i];
    return writes.size();
}
Registry& reg(uint32_t) { return g_items; }
const Entry* Registry::get(uint32_t id) const {
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &entries[it->second];
}
const Field* Schema::find(const std::string&) const { return nullptr; }
KilnContentValue decode(const Field&, const uint8_t* b) {
    KilnContentValue v{};
    uint32_t x = 0;
    std::memcpy(&x, b, 4);
    v.type = KILN_CV_INT, v.i = x;
    return v;
}
std::string show(const Field&, const KilnContentValue& v) { return std::to_string(v.i); }
bool patch_code_byte(uintptr_t, uint8_t, uint8_t) { return false; }
}  // namespace kc

namespace {
using namespace kc;
int checks = 0;
void check(bool yes, const std::string& name) {
    if (!yes) throw std::runtime_error(name);
    ++checks;
}
void check(bool yes, const char* name) {
    if (!yes) throw std::runtime_error(name);
    ++checks;
}

constexpr KilnOwner PACK = 11, PACK2 = 12, REG = 33, CODE = 44;
constexpr uintptr_t F1 = 0x1000, F2 = 0x1010, F3 = 0x1020;
constexpr uint64_t kPackSeq = (1ull << 44) | (1ull << 32) | 1;

uint32_t at(uintptr_t a) {
    uint32_t v = 0;
    rd(a, &v, 4);
    return v;
}
void mem(uintptr_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) g_mem[a + i] = static_cast<uint8_t>(v >> (8 * i));
}
bool put(KilnOwner o, uint64_t seq, uintptr_t a, uint32_t v, const char* source = "test") {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    return store_put(o, seq, 1, 1, 0, -1, a, 4, b, source);
}
// a relative registry layer: value below * mul (+ add)
bool put_mul(uintptr_t a, uint32_t mul, uint32_t add = 0, uint32_t ordinal = 1) {
    LayerEval ev = [mul, add](const uint8_t* below, uint8_t n, uint8_t* out, std::string*) {
        uint32_t v = 0;
        std::memcpy(&v, below, n);
        v = v * mul + add;
        std::memcpy(out, &v, n);
        return true;
    };
    LayerModScope scope("bigstacks");
    return store_put_eval(REG, registry_seq(ordinal), 1, 1, 0, -1, a, 4, std::move(ev), "bigstacks: content/a.json");
}
void flush() {
    std::string e;
    store_flush(&e);
}
void fresh() {
    std::string e;
    store_reset(false, &e);
    g_mem.clear();
    for (uintptr_t a : {F1, F2, F3}) mem(a, 10);  // the game's values
    g_items = Registry{};
    g_items.label = "items";
    Entry en;
    en.id = 1, en.name = "Material_T1_Wood_Logs";
    g_items.entries.push_back(en);
    g_items.by_id[1] = 0;
    g_items.schema.fields.resize(1);
    g_items.schema.fields[0].path = "maxStackSize";
    g_items.schema.fields[0].bytes = 4;
}

// the band: packs < registry < code, whatever order they arrive in
void band_order() {
    fresh();
    check(is_registry_seq(registry_seq(7)) && !is_registry_seq(kPackSeq) && !is_registry_seq(next_code_seq()), "band test");
    check(registry_seq(0) > kPackSeq && registry_seq(0xFFFFFFFFu) < kCodeSeq, "the band lies between packs and code");
    put(CODE, next_code_seq(), F1, 99);
    put(REG, registry_seq(1), F1, 50);
    put(PACK, kPackSeq, F1, 30);
    flush();
    check(at(F1) == 99, "code wins over the registry and packs");
    store_drop_owner(CODE);
    flush();
    check(at(F1) == 50, "the registry wins over packs");
    store_drop_owner(REG);
    flush();
    check(at(F1) == 30, "the pack's value again");
}

// a relative layer is evaluated over the value below it at every flush: the original, then a pack's, then a reloaded pack's
void relative_over_the_value_below() {
    fresh();
    check(put_mul(F1, 2), "put an evaluated layer");
    flush();
    check(at(F1) == 20, "x2 over the game's 10");
    put(PACK, kPackSeq, F1, 30);  // a pack arrives under it (the pack's own value is absolute)
    flush();
    check(at(F1) == 60, "x2 over the pack's 30");
    store_drop_owner(PACK);  // a hot reload: the pack's layers off, the edited pack applied again
    put(PACK, kPackSeq, F1, 50);
    flush();
    check(at(F1) == 100, "x2 over the reloaded pack's 50");
    store_drop_owner(PACK);  // the pack removed
    flush();
    check(at(F1) == 20, "x2 over the game's value once the pack is gone");
    uint8_t b[4];
    check(store_effective(F1, 4, b) && b[0] == 20, "store_effective gives the folded value");
    put(PACK2, kPackSeq + 1, F1, 7);  // before any flush
    check(store_effective(F1, 4, b) && b[0] == 14 && at(F1) == 20, "store_effective folds before the flush writes");
    put(CODE, next_code_seq(), F1, 5);
    flush();
    check(at(F1) == 5, "code over an evaluated layer");
    store_drop_owner(REG);
    store_drop_owner(CODE);
    flush();
    check(at(F1) == 7, "the evaluated layer leaves no residue");
}

// an evaluation that gives no value holds the value below
void failed_evaluation_holds_the_value_below() {
    fresh();
    LayerEval ev = [](const uint8_t*, uint8_t, uint8_t*, std::string* why) {
        *why = "does not fit";
        return false;
    };
    put(PACK, kPackSeq, F1, 30);
    store_put_eval(REG, registry_seq(1), 1, 1, 0, -1, F1, 4, std::move(ev), "x");
    flush();
    check(at(F1) == 30, "the pack's value stays");
}

// conflicts name the evaluated value
void conflicts_show_the_evaluated_value() {
    fresh();
    put(PACK, kPackSeq, F1, 30, "oldstacks: content/a.json");
    put_mul(F1, 4);
    flush();
    const auto cs = store_conflicts();
    check(cs.size() == 1 && cs[0].winner == "bigstacks: content/a.json" && cs[0].winner_value == "120" && cs[0].loser_value == "30",
          "registry over pack: one conflict with the evaluated 120");
}

// another writer wrote the slot after Kiln's layer: the registry write is skipped, reported, and the slot stays with that writer
void another_writer_is_never_overwritten() {
    fresh();
    put(PACK, kPackSeq, F1, 30);
    put_mul(F1, 2);
    flush();
    check(at(F1) == 60, "start: 60");
    mem(F1, 777);  // a ShroudForge run-time write (kiln mode), or any other writer
    store_mark_evals();  // the registry's layer evaluated again (a formula input changed, a recomposition)
    flush();
    check(at(F1) == 777, "the other writer's value stays");
    auto sk = store_skipped();
    check(sk.size() == 1, "one skipped write");
    check(sk[0].what == "items Material_T1_Wood_Logs.maxStackSize" && sk[0].mod == "bigstacks" && !sk[0].revert, "it names the field and the mod");
    check(sk[0].text.find("was written by another writer after Kiln's layer: Kiln's edit not applied") != std::string::npos, sk[0].text.c_str());
    put_mul(F1, 3);  // the registry puts its layer again (a recomposition): still skipped
    flush();
    check(at(F1) == 777, "the slot stays with the other writer");
    check(store_skipped().size() == 1, "reported once per slot");
    uint8_t b[4];
    check(store_effective(F1, 4, b) && b[0] == 30, "the registry layer is left out of the fold");
    store_drop_owner(PACK);  // an older path's own change is written as today (the pack's reload)
    put(PACK, kPackSeq, F1, 41);
    flush();
    check(at(F1) == 41 && store_skipped().size() == 1, "a pack's change on that slot is written as it is today");
}

// review finding 7: another writer detected in the same flush as an older path's change: the pack's new value is written
void detection_in_the_flush_of_a_pack_change() {
    fresh();
    put(PACK, kPackSeq, F1, 30);
    put_mul(F1, 2);
    flush();
    mem(F1, 777);
    store_drop_owner(PACK);  // a pack reload: the pack's layer dropped and put again with its new value
    put(PACK, kPackSeq, F1, 50);
    flush();
    check(store_skipped().size() == 1, "the registry's write is skipped and reported");
    check(at(F1) == 50, "the old pack's new value is written as today (memory " + std::to_string(at(F1)) + ")");
}

// the revert compares too: dropping a registry layer over a value another writer set since leaves that value
void a_revert_is_compared_too() {
    fresh();
    put_mul(F2, 5);
    flush();
    check(at(F2) == 50, "x5");
    mem(F2, 555);
    store_drop_owner(REG);
    flush();
    check(at(F2) == 555, "the revert was skipped");
    auto sk = store_skipped();
    check(sk.size() == 1 && sk[0].revert && sk[0].text.find("Kiln's revert not applied") != std::string::npos, "the skipped revert is reported");
}

// no other writer: no skip; old paths keep today's behaviour (a pack overwrites whatever is there)
void old_paths_unchanged() {
    fresh();
    put_mul(F1, 2);
    flush();
    put(PACK, kPackSeq, F1, 30);
    flush();
    check(at(F1) == 60 && store_skipped().empty(), "no other writer: re-evaluated, nothing skipped");
    put(PACK, kPackSeq, F3, 30);
    flush();
    mem(F3, 888);
    store_drop_owner(PACK);
    put(PACK, kPackSeq, F3, 31);
    flush();
    check(at(F3) == 31 && store_skipped().empty(), "a pack-only slot is written as today");
}

// store_mark_evals makes every evaluated slot re-evaluate (dependencies on other slots: formulas)
void mark_evals_re_evaluates() {
    fresh();
    uint32_t dep = 3;
    LayerEval ev = [&dep](const uint8_t* below, uint8_t n, uint8_t* out, std::string*) {
        uint32_t v = 0;
        std::memcpy(&v, below, n);
        v *= dep;
        std::memcpy(out, &v, n);
        return true;
    };
    store_put_eval(REG, registry_seq(1), 1, 1, 0, -1, F1, 4, std::move(ev), "x");
    flush();
    check(at(F1) == 30, "x3");
    dep = 4;
    flush();
    check(at(F1) == 30, "a clean slot is not evaluated again");
    check(store_mark_evals() == 1, "one evaluated slot marked");
    flush();
    check(at(F1) == 40, "x4 after the mark");
}

// store_below_registry gives the value under the registry band; evaluated layers reading each other do not recurse forever
void below_registry_and_cycles() {
    fresh();
    put(PACK, kPackSeq, F1, 30);
    put_mul(F1, 2);
    uint8_t b[4];
    check(store_below_registry(F1, 4, b) && b[0] == 30, "below the registry: the pack's 30");
    check(store_effective(F1, 4, b) && b[0] == 60, "effective: 60");
    LayerEval a = [](const uint8_t* below, uint8_t n, uint8_t* out, std::string*) {
        uint8_t o[4];
        store_effective(F3, 4, o);
        std::memcpy(out, below, n);
        out[0] = static_cast<uint8_t>(out[0] + o[0]);
        return true;
    };
    LayerEval c = [](const uint8_t* below, uint8_t n, uint8_t* out, std::string*) {
        uint8_t o[4];
        store_effective(F2, 4, o);
        std::memcpy(out, below, n);
        out[0] = static_cast<uint8_t>(out[0] + o[0]);
        return true;
    };
    store_put_eval(REG, registry_seq(2), 1, 1, 0, -1, F2, 4, std::move(a), "x");
    store_put_eval(REG, registry_seq(3), 1, 1, 0, -1, F3, 4, std::move(c), "x");
    flush();
    check(at(F2) != 0 && at(F3) != 0, "a cycle ends (bounded depth)");
}

// review nit: an evaluated layer that reads another field of its entry (a formula) is evaluated again when another layer of
// that field changes (a code layer on the level), without store_mark_evals
void entry_change_re_evaluates() {
    fresh();
    LayerEval ev = [](const uint8_t* below, uint8_t n, uint8_t* out, std::string*) {
        uint8_t other[4];
        store_effective(F2, 4, other);
        std::memcpy(out, below, n);
        out[0] = static_cast<uint8_t>(below[0] + other[0]);
        return true;
    };
    store_put_eval(REG, registry_seq(1), 1, 1, 0, -1, F1, 4, std::move(ev), "x");
    flush();
    check(at(F1) == 20, "10 + 10");
    put(CODE, next_code_seq(), F2, 5);  // another field of the same entry (kind 1, id 1) changes
    flush();
    check(at(F1) == 15, "evaluated again: 10 + 5 (now " + std::to_string(at(F1)) + ")");
}

// a world reset forgets skipped writes and other writers
void reset_forgets() {
    fresh();
    put_mul(F1, 2);
    flush();
    mem(F1, 1);
    store_mark_evals();
    flush();
    check(store_skipped().size() == 1, "skipped");
    fresh();
    check(store_skipped().empty(), "a reset forgets it");
    put_mul(F1, 2);
    flush();
    check(at(F1) == 20, "a new world writes again");
}

// a content transaction's rollback reads evaluated layers back as folded
void rollback_reads_folded_values() {
    fresh();
    put(PACK, kPackSeq, F1, 30);
    put_mul(F1, 2);
    flush();
    uint64_t id = 0;
    check(store_txn_begin(PACK, &id), "begin");
    put(PACK, kPackSeq, F1, 40);
    flush();
    check(at(F1) == 80, "80");
    uint32_t n = 0;
    std::string why;
    check(store_txn_rollback(id, nullptr, nullptr, &n, &why) == TxnOutcome::Restored, ("restored: " + why).c_str());
    check(at(F1) == 60, "back to x2 over 30");
}
}  // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"band_order", band_order},
        {"relative_over_the_value_below", relative_over_the_value_below},
        {"failed_evaluation_holds_the_value_below", failed_evaluation_holds_the_value_below},
        {"conflicts_show_the_evaluated_value", conflicts_show_the_evaluated_value},
        {"another_writer_is_never_overwritten", another_writer_is_never_overwritten},
        {"detection_in_the_flush_of_a_pack_change", detection_in_the_flush_of_a_pack_change},
        {"a_revert_is_compared_too", a_revert_is_compared_too},
        {"old_paths_unchanged", old_paths_unchanged},
        {"mark_evals_re_evaluates", mark_evals_re_evaluates},
        {"below_registry_and_cycles", below_registry_and_cycles},
        {"entry_change_re_evaluates", entry_change_re_evaluates},
        {"reset_forgets", reset_forgets},
        {"rollback_reads_folded_values", rollback_reads_folded_values},
    };
    int failed = 0;
    for (auto& [name, fn] : tests) {
        try {
            fn();
            std::printf("ok   %s\n", name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s: %s\n", name, e.what());
        }
    }
    std::printf("registry store: %d checks, %d failed tests\n", checks, failed);
    return failed ? 1 : 0;
}
