// SPDX-License-Identifier: MIT
// Reuse the existing host seams and assertions; each invocation has a fresh process-wide store.
// KILN_ASSETS_VFX_BASELINE compiles only the exact boundary test against baseline store.cpp.
#define main previous_store_main
#include "test_store.cpp"
#undef main
#include <filesystem>
#include <fstream>
#ifndef KILN_ASSETS_VFX_BASELINE
#include "vfx_preflight.h"

// Production hook body; just the original call and guarded memory copy are host seams.
namespace vfx_hook {
using namespace ka;
bool readable = true, error = false;
bool rd(uintptr_t address, void* out, size_t bytes) {
    if (!readable) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
    return true;
}
TypeList* orig_by_type(TypeList* out, uintptr_t S, uint32_t type, uint32_t*) {
    *out = {};
    game_type_list(S, type, &out->keys, &out->count);
    out->error = error ? 0x12 : 0;
    return out;
}
#include "vfx_copy_keys.inc"
#define __fastcall
#include "vfx_by_type.inc"
#undef __fastcall
}  // namespace vfx_hook
#endif

namespace {
constexpr uint32_t vtype = 0xe64ee036u;
Key vk(uint32_t id, uint32_t part = 0) {
    Key k{};
    std::memcpy(k.g, &id, 4);
    k.g[15] = 0x77;
    k.type = vtype;
    k.part = part;
    return k;
}
#ifndef KILN_ASSETS_VFX_BASELINE
void word(std::vector<uint8_t>& b, size_t at, uint32_t n) { std::memcpy(b.data() + at, &n, 4); }
void write(const std::filesystem::path& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), b.size());
    if (!f) std::abort();
}
// Synthetic metadata: two type bundles, nonidentity order, and a repeated VFX alias. No resource bytes.
std::vector<uint8_t> archive() {
    constexpr uint32_t n = 1575, types = 0x90, bundles = types + 8, order = bundles + 24, keys = order + (n + 2) * 4;
    std::vector<uint8_t> b(keys + (n + 1) * 32, 0);
    std::memcpy(b.data(), "KFC4", 4);
    word(b, 4, b.size());
    word(b, 8, 12);
    const auto loc = [&](size_t at, uint32_t off, uint32_t count) { word(b, at, off - at); word(b, at + 4, count); };
    loc(0x38, order, n + 2); loc(0x60, keys, n + 1); loc(0x78, types, 2); loc(0x80, bundles, 2);
    word(b, types, 0x12345678); word(b, types + 4, vtype);
    word(b, bundles + 8, 1); word(b, bundles + 12 + 4, 1); word(b, bundles + 12 + 8, n + 1);
    Key other = vk(0); other.type = 0x12345678;
    std::memcpy(b.data() + keys, &other, sizeof other);
    for (uint32_t i = 1; i <= n; ++i) {
        const Key k = vk(i);
        std::memcpy(b.data() + keys + i * 32, &k, sizeof k);
        word(b, order + i * 4, n + 1 - i);
    }
    word(b, order + (n + 1) * 4, 1);  // duplicate alias
    return b;
}
#endif
void stock() {
    for (uint32_t i = 1; i <= 1575; ++i) g_game[vk(i)] = {i, "STOCK"};
}
#ifndef KILN_ASSETS_VFX_BASELINE
void start(const std::filesystem::path& p, uint32_t capacity = 2048) {
    write(p, archive());
    vfx_preflight_start(p.wstring(), capacity);
}
KilnAssetsVfxBudget budget() {
    KilnAssetsVfxBudget b{sizeof b, 0, 0, 0};
    check(op_vfx_budget(&b) == KILN_OK, "VFX census is available");
    return b;
}
#endif
std::vector<KilnAssetKey> listed() {
    std::vector<KilnAssetKey> v(70000);
    size_t n = 0;
    check(op_resource_list(vtype, v.data(), v.size(), &n) == KILN_OK, "actual VFX list is readable");
    v.resize(n);
    return v;
}
void boundary(uint32_t raw_flags) {
    check(put(100, vk(10000), "RAW", raw_flags) == KILN_OK, "one external raw VFX accepted before ResourceInit");
    size_t accepted = 0, refused = 0;
    for (uint32_t i = 0; i < 473; ++i) {
        const auto r = put(200, vk(20000 + i), "COOKED", KILN_ASSET_PUT_NEW);
        accepted += r == KILN_OK;
        refused += r == KILN_ERR_UNAVAILABLE;
    }
    check(accepted == 472 && refused == 1, "1575 stock + 1 raw leaves 472 of 473 hypothetical cooked adds (2049th refused)");
#ifndef KILN_ASSETS_VFX_BASELINE
    check(budget().reserved == 473, "pre-bind budget shares all raw/cooker reservations");
    check(logged(0, "owner 200 resource", "VFX capacity exhausted"), "full refusal names owner/key and reason");
#endif
    g_S = 1; on_resource_system_ready(1);
    check(listed().size() == 2048, "final merged list stays exactly at the stock 2048 cap");
    const auto rejected = ak(vk(20472));
    const void* ptr = nullptr; size_t n = 0;
    check(op_resource_data(&rejected, &ptr, &n) == KILN_ERR_NOT_FOUND, "refused resource is not readable or served");
    check(put(201, vk(30000), "LATE", 0) == KILN_ERR_UNAVAILABLE, "late ANY callers share the exhausted budget");
}
#ifndef KILN_ASSETS_VFX_BASELINE
void retries() {
    check(put(100, vk(10000), "A", 0) == KILN_OK && put(101, vk(10000), "B", 0) == KILN_OK,
          "overlapping owners reserve one distinct ANY key");
    check(put(100, vk(10000), "A2", KILN_ASSET_PUT_NEW) == KILN_OK && budget().reserved == 1,
          "replacement by same owner does not double reserve");
    op_revert_owner(100);
    check(budget().reserved == 1, "another owner keeps the shared reservation");
    op_revert_owner(101);
    check(budget().reserved == 0, "pre-bind prepare rollback releases the reservation");
    check(put(102, vk(10001), "RETRY", 0) == KILN_OK, "prepare retry can use released room");
    store_owner_retired(102); store_drain_retired();
    check(budget().reserved == 0, "retired unbound owner releases room");
    while (!store_cleanup_idle()) std::this_thread::yield();
    check(put(102, vk(10001), "REVOKED", 0) == KILN_ERR_REVOKED, "retired owner cannot reserve again");
    check(put(103, vk(10002), "BOUND", 0) == KILN_OK, "another prepare reserves the freed room");
    g_S = 1; on_resource_system_ready(1);
    op_revert_owner(103);
    check(budget().reserved == 1 && listed().size() == 1575, "bound removal retains lifetime room while removing the live listing");
    check(put(104, vk(10003), "NO ROOM", 0) == KILN_ERR_UNAVAILABLE, "a removed bound key cannot lend its slot to a different GUID");
    check(put(104, vk(10002), "AGAIN", 0) == KILN_OK && put(105, vk(10002), "WINNER", 0) == KILN_OK,
          "the same lifetime key can be restored by overlapping owners");
    check(listed().size() == 1576 && op_resource_new_count(vtype) == 1, "duplicate owners enumerate and count their key once");
    check(read(vk(10002)) == "WINNER", "overlap preserves normal last-owner precedence");
    op_revert_owner(105);
    check(read(vk(10002)) == "AGAIN", "retiring winner restores the earlier owner's bytes");
}
void originals() {
    check(put(100, vk(1), "OVERRIDE", 0) == KILN_OK && put(101, vk(2), "EXPLICIT", KILN_ASSET_PUT_OVERRIDE) == KILN_OK,
          "stock replacements accepted at full capacity in ANY and override modes");
    check(put(102, vk(3), "SKIP", KILN_ASSET_PUT_NEW) == KILN_OK, "NEW stock key keeps existing skip semantics");
    check(put(103, vk(90000), "ABSENT", KILN_ASSET_PUT_OVERRIDE) == KILN_OK && budget().reserved == 0,
          "missing override cannot reserve or synthesize a new key");
    g_S = 1; on_resource_system_ready(1);
    check(listed().size() == 1575 && read(vk(1)) == "OVERRIDE" && read(vk(3)) == "STOCK",
          "stock replacements add no enumeration aliases; skipped NEW leaks no bytes");
    const auto absent = ak(vk(90000)); const void* p; size_t n;
    check(op_resource_data(&absent, &p, &n) == KILN_ERR_NOT_FOUND, "missing override leaks no bytes through resource_data");
    g_fake_lookup_missing = true;
    check(put(104, vk(4), "BAD LOOKUP", 0) == KILN_ERR_UNAVAILABLE && by_handle(4) == "<not served>",
          "failed stock lookup never synthesizes or serves an original key");
    const auto failed = ak(vk(4));
    check(op_resource_data(&failed, &p, &n) == KILN_ERR_NOT_FOUND, "failed stock bind does not leak the rejected override bytes");
    g_fake_lookup_missing = false;
    Key other = vk(91000); other.type = 0x12345678;
    check(put(100, other, "ORDINARY", 0) == KILN_OK && read(other) == "ORDINARY", "non-VFX resource behavior is unchanged");
}
void actual_enumeration() {
    check(put(100, vk(10000), "OUR", 0) == KILN_OK, "one new VFX admitted from disk census");
    g_S = 1; on_resource_system_ready(1);
    check(listed().size() == 1576, "initial actual merge includes our addition");
    g_game[vk(11000)] = {11000, "THIRD PARTY"};
    check(budget().reserved == 2, "budget includes foreign native additions beyond the disk census");
    check(listed().size() == 1576 && read(vk(11000)) == "THIRD PARTY",
          "extra native original consumes room: final guard omits our additions and preserves other-loader keys");
    check(put(101, vk(12000), "LATE", 0) == KILN_ERR_UNAVAILABLE, "late put uses actual native keys beyond the disk census");
    std::vector<Key> merged;
    check(vfx_merge({vk(1), vk(1), vk(2)}, {vk(2), vk(3), vk(3)}, 3, &merged) && merged.size() == 3,
          "original aliases and overlapping additions count and enumerate once");
    check(!vfx_merge({vk(1), vk(2), vk(3)}, {vk(4)}, 2, &merged) && merged.size() == 3,
          "an already-overfull foreign list is preserved whole without our additions");
    g_fake_vfx_readable = false;
    check(put(101, vk(12001), "UNREADABLE", 0) == KILN_ERR_UNAVAILABLE, "unreadable actual enumeration fails closed on new VFX");
}
void bind_fallout(bool readable) {
    check(put(100, vk(10000), "PENDING", 0) == KILN_OK, "boot raw addition reserves its slot");
    check(put(101, vk(1), "STOCK OVERRIDE", KILN_ASSET_PUT_OVERRIDE) == KILN_OK, "stock override is prepared alongside it");
    if (readable) g_game[vk(11000)] = {11000, "THIRD PARTY"};
    g_fake_vfx_readable = readable;
    g_S = 1; on_resource_system_ready(1);
    const auto k = ak(vk(10000)); const void* p; size_t n;
    check(op_resource_data(&k, &p, &n) == KILN_ERR_NOT_FOUND, "failed bind is neither synthesized nor readable");
    check(read(vk(1)) == "STOCK OVERRIDE", "failed new bind preserves stock replacements");
    check(logged(0, "owner 100 resource", "refused"), "bind fallout logs the refused owner/key");
    g_fake_vfx_readable = true;
    check(listed().size() == (readable ? 1576 : 1575), "bind fallout preserves the complete original listing without the refused key");
}

KilnResult named_put(KilnOwner owner, const Key& key, const std::string& mod, uint32_t flags = 0) {
    const KilnAssetKey k = ak(key);
    return op_vfx_resource_put(owner, {mod.data(), mod.size()}, &k, "COOKED", 6, flags);
}
std::vector<Key> hook_list() {
    TypeList out{};
    check(vfx_hook::hk_by_type(&out, 1, vtype, nullptr) == &out && !out.error, "production VFX type-list hook returns a readable list");
    std::vector<Key> keys(out.count);
    for (size_t i = 0; i < keys.size(); ++i) std::memcpy(&keys[i], out.keys + i * 32, sizeof(Key));
    return keys;
}
std::vector<ResourceRegistration> registrations() {
    std::vector<ResourceRegistration> out;
    ReadGuard reading;
    if (const ResView* view = g_res.load()) {
        std::vector<Key> added;
        view->new_keys(vtype, &added);
        vfx_omitted(*view, added, {}, &out);
    }
    return out;
}
void reentrant_reporter() {
    kiln::boot::g_fault_reported = [] {
        {
            std::unique_lock pending(g_pending_mu, std::try_to_lock);
            check(pending.owns_lock(), "activation callback runs outside the pending mutex");
        }
        {
            ReadGuard reading;
            const ResView* view = g_res.load();
            std::unique_lock merged(view->merged_mu, std::try_to_lock);
            check(merged.owns_lock(), "activation callback runs outside the merged mutex");
        }
        Key other = vk(99000); other.type = 0x12345678;
        // Republishing waits for readers, so this also detects a callback inside the hook's ReadGuard.
        check(put(900, other, "REPORTER", 0) == KILN_OK && read(other) == "REPORTER",
              "activation callback can republish the store after all reader guards have left");
    };
}
bool fault(const std::string& mod, const Key& k, const std::string& why) {
    for (const auto& f : kiln::boot::g_faults)
        if (f.mod == mod && f.kind == "assets" && f.key == key_str(k) && f.why.find(why) != std::string::npos) return true;
    return false;
}
void provenance_bind() {
    std::string mod = "cooker.required";
    check(named_put(200, vk(10000), mod) == KILN_OK, "cooker puts a named resource before binding");
    mod.assign(mod.size(), 'x');
    g_boot_required_owner[201] = "raw.required";
    check(put(201, vk(10000), "RAW", 0) == KILN_OK && put(202, vk(10000), "NATIVE", 0) == KILN_OK,
          "required raw and unknown native sources share the same pending key");
    g_game[vk(11000)] = {11000, "FOREIGN"};
    reentrant_reporter();
    g_S = 1; on_resource_system_ready(1);
    check(kiln::boot::g_faults.size() == 2 && fault("cooker.required", vk(10000), "capacity") &&
              fault("raw.required", vk(10000), "capacity"),
          "late bind refusal retains copied cooker mod and resolves required raw owner in production reporter");
    check(logged(0, "cooker.required owner 200 resource " + key_str(vk(10000)), "capacity") &&
              logged(0, "raw.required owner 201 resource " + key_str(vk(10000)), "capacity") &&
              logged(0, "<unknown> owner 202 resource " + key_str(vk(10000)), "capacity"),
          "all affected sources log mod identity or unknown, owner, key and refusal reason");
    on_resource_system_ready(1);
    check(kiln::boot::g_faults.size() == 2, "repeat binding does not duplicate activation faults");
    check(hook_list().size() == 1576 && read(vk(11000)) == "FOREIGN" && read(vk(10000)) == "<result 2>",
          "late bind refusal preserves originals and foreign keys without serving refused bytes");
}
void provenance_final() {
    check(named_put(200, vk(10000), "cooker.required") == KILN_OK, "named cooker addition prepared");
    g_boot_required_owner[201] = "raw.required";
    check(put(201, vk(10001), "RAW", 0) == KILN_OK && put(202, vk(10002), "NATIVE", 0) == KILN_OK &&
              named_put(203, vk(10003), "preserved.mod") == KILN_OK, "raw, native and overlapping-original candidates prepared");
    g_S = 1; on_resource_system_ready(1);
    check(hook_list().size() == 1579 && kiln::boot::g_faults.empty(), "initial actual hook merge fits without any faults");
    g_game[vk(10003)] = {10003, "FOREIGN ALIAS"};
    g_game[vk(11000)] = {11000, "FOREIGN"};
    reentrant_reporter();
    const auto keys = hook_list();
    VfxKeys present(keys.begin(), keys.end());
    bool originals_present = true;
    for (const auto& [k, unused] : g_game) originals_present &= present.count(k) != 0;
    check(keys.size() == 1577 && originals_present && !present.count(vk(10000)) && !present.count(vk(10001)) && !present.count(vk(10002)),
          "overflow leaves every original and foreign key, with no Kiln-only additions");
    check(kiln::boot::g_faults.size() == 2 && fault("cooker.required", vk(10000), "exceeds capacity") &&
              fault("raw.required", vk(10001), "exceeds capacity"),
          "final hook omission reports named cooker and required raw sources only for absent keys");
    check(logged(0, "<unknown> owner 202 resource " + key_str(vk(10002)), "exceeds capacity"),
          "unknown native omission stays a named owner/key diagnostic");
    hook_list(); hook_list(); listed();
    check(kiln::boot::g_faults.size() == 2, "repeat hooks and public list share per-registration omission deduplication");
}
void provenance_unreadable() {
    check(named_put(200, vk(10000), "cooker.required") == KILN_OK, "named addition prepared for unreadable enumeration");
    g_S = 1; on_resource_system_ready(1);
    reentrant_reporter();
    vfx_hook::readable = false;
    check(hook_list().size() == 1575 && fault("cooker.required", vk(10000), "unreadable"),
          "unreadable original hook list preserves original output and reports absent addition");
    vfx_hook::readable = true; vfx_hook::error = true;
    TypeList out{};
    vfx_hook::hk_by_type(&out, 1, vtype, nullptr);
    check(out.error == 0x12 && out.count == 1575, "original enumeration error is preserved");
    g_fake_vfx_readable = false;
    size_t count = 0;
    check(op_resource_list(vtype, nullptr, 0, &count) == KILN_ERR_UNAVAILABLE && kiln::boot::g_faults.size() == 1,
          "unreadable hook/public-list failures report the same omission only once");
    g_fake_vfx_readable = true; vfx_hook::error = false;
    check(hook_list().size() == 1576, "recovered enumeration can serve the addition again");
}
void provenance_retry() {
    // S can precede assets' on_ready and every prepare. Empty early binds must retain no fault provenance.
    check(store_note_resource_system(1), "resource system appears before prepare");
    on_resource_system_ready(1); on_resource_system_ready(1);
    check(named_put(200, vk(10000), "old.prepare") == KILN_OK, "early-S prepare binds immediately");
    const auto old = registrations();
    check(named_put(200, vk(10000), "replacement.prepare") == KILN_OK, "same owner's re-registration takes new provenance");
    report_vfx_omissions(old, "synthetic final omission");
    check(kiln::boot::g_faults.empty(), "stale enumeration cannot attribute an old omission to the replacement mod");
    const auto replaced = registrations();
    op_revert_owner(200);
    check(named_put(201, vk(10000), "retry.prepare") == KILN_OK, "prepare rollback permits a new owner of the same key");
    report_vfx_omissions(replaced, "synthetic final omission");
    check(kiln::boot::g_faults.empty(), "rolled-back owner's omission cannot fault the retry owner");
    reentrant_reporter();
    report_vfx_omissions(registrations(), "synthetic final omission");
    report_vfx_omissions(registrations(), "synthetic final omission");
    check(kiln::boot::g_faults.size() == 1 && fault("retry.prepare", vk(10000), "synthetic"), "current registration reports once");
    check(named_put(201, vk(10000), "retry.again") == KILN_OK, "a fresh put resets omission deduplication and provenance");
    report_vfx_omissions(registrations(), "synthetic final omission");
    check(kiln::boot::g_faults.size() == 2 && fault("retry.again", vk(10000), "synthetic"), "fresh registration can report the same reason again");
    check(put(201, vk(10000), "NATIVE", 0) == KILN_OK, "legacy put replaces the resource without cooker identity");
    report_vfx_omissions(registrations(), "synthetic final omission");
    check(kiln::boot::g_faults.size() == 2 && logged(0, "<unknown> owner 201 resource", "synthetic"),
          "legacy replacement clears stale cooker mod provenance and remains a native diagnostic");
    const KilnAssetKey k = ak(vk(10000));
    check(op_vfx_resource_put(201, {nullptr, 1}, &k, "X", 1, 0) == KILN_ERR_INVALID_ARGUMENT,
          "named put rejects a missing nonempty mod span");
}
void unknown(const std::filesystem::path& path) {
    vfx_preflight_start(std::filesystem::path(path.string() + ".missing").wstring(), 2048);
    KilnAssetsVfxBudget b{sizeof b, 99, 99, 99};
    check(op_vfx_budget(&b) == KILN_ERR_UNAVAILABLE && b.capacity == 99, "unknown census budget fails closed without inventing counts");
    check(put(100, vk(10000), "NEW", 0) == KILN_ERR_UNAVAILABLE, "unknown census refuses boot ANY additions");
    check(put(100, vk(1), "OVERRIDE", KILN_ASSET_PUT_OVERRIDE) == KILN_OK, "explicit stock override remains usable without census");
    g_S = 1; on_resource_system_ready(1);
    check(read(vk(1)) == "OVERRIDE", "stock resource survives census failure");
    check(put(101, vk(2), "ANY", 0) == KILN_OK && read(vk(2)) == "ANY", "proven stock ANY replacement remains usable after ResourceInit");
}
void capacity() {
    KilnAssetsVfxBudget small{15, 9, 9, 9};
    check(op_vfx_budget(&small) == KILN_ERR_INVALID_ARGUMENT && small.capacity == 9 &&
              op_vfx_budget(nullptr) == KILN_ERR_INVALID_ARGUMENT, "budget checks its size and pointer");
    check(budget().capacity == 1576, "stock sigdb capacity may differ from 2048");
    check(op_vfx_set_capacity(100, 8192) == KILN_ERR_INVALID_ARGUMENT && op_vfx_set_capacity(0, 4096) == KILN_ERR_INVALID_ARGUMENT,
          "capacity setter permits only 2048/4096 and a nonzero owner");
    check(op_vfx_set_capacity(100, 4096) == KILN_OK && budget().capacity == 4096,
          "boot setter records an installed 4096 capacity");
    check(op_vfx_set_capacity(100, 2048) == KILN_OK && budget().capacity == 2048, "boot setter can record stock 2048");
    g_S = 1;
    check(op_vfx_set_capacity(100, 4096) == KILN_ERR_UNAVAILABLE, "S present rejects capacity changes before ready callback");
    g_S = 0; on_resource_system_ready(0);
    check(op_vfx_set_capacity(100, 4096) == KILN_ERR_UNAVAILABLE && budget().capacity == 2048,
          "resources-ready started rejects capacity changes even without S");
    check(store_note_resource_system(1) && !store_note_resource_system(2) && g_S.load() == 1,
          "first resource-system publication is serialized with the capacity gate");
}
void concurrent() {
    g_S = 1; on_resource_system_ready(1);
    std::atomic<unsigned> accepted{0}, refused{0};
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (uint32_t i = 0; i < 20; ++i) {
                const auto r = put(100 + t, vk(10000 + t * 20 + i), "LATE", 0);
                accepted += r == KILN_OK; refused += r == KILN_ERR_UNAVAILABLE;
            }
        });
    for (auto& t : threads) t.join();
    check(accepted == 25 && refused == 135 && listed().size() == 1600,
          "concurrent late ANY puts atomically share exactly the remaining 25 slots");
}
void metadata(const std::filesystem::path& path) {
    auto b = archive(); VfxKeys keys; std::string why;
    check(read_vfx_census(path, &keys, &why) && keys.size() == 1575 && keys.count(vk(1)) && keys.count(vk(1575)),
          "ordered bundle reads original keys and deduplicates stock aliases");
    auto bad = b; word(bad, 0x64, 0xffffffff); write(path, bad);
    check(!read_vfx_census(path, &keys, &why) && keys.empty(), "oversized key metadata rejected before allocation");
    bad = b; word(bad, 0x84, 3); write(path, bad);
    check(!read_vfx_census(path, &keys, &why), "mismatched parallel bundle tables rejected");
    bad = b; word(bad, 0x3c, 1); write(path, bad);
    check(!read_vfx_census(path, &keys, &why), "bundle order range past end rejected");
    bad = b; uint32_t off; std::memcpy(&off, bad.data() + 0x38, 4); word(bad, 0x38 + off + 4, 0xffffffff); write(path, bad);
    check(!read_vfx_census(path, &keys, &why), "invalid ordered key index rejected");
    bad = b; bad.resize(bad.size() - 1); write(path, bad);
    check(!read_vfx_census(path, &keys, &why), "truncated metadata rejected");
}
#endif
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    KilnApi api{}; api.log = &log_line; g_api = &api;
    stock();
    const std::string mode = argv[1];
    const std::filesystem::path path = argv[2];
#ifndef KILN_ASSETS_VFX_BASELINE
    if (mode != "unknown") start(path, mode == "boundary-any" || mode == "boundary-new" ? 2048 :
                                          mode == "originals" ? 1575 : mode == "concurrent" ? 1600 :
                                          mode == "provenance-final" ? 1579 : 1576);
#endif
    if (mode == "boundary-any" || mode == "boundary-new") boundary(mode == "boundary-any" ? 0 : KILN_ASSET_PUT_NEW);
#ifndef KILN_ASSETS_VFX_BASELINE
    else if (mode == "retry") retries();
    else if (mode == "originals") originals();
    else if (mode == "actual") actual_enumeration();
    else if (mode == "bind") bind_fallout(true);
    else if (mode == "bind-unreadable") bind_fallout(false);
    else if (mode == "unknown") unknown(path);
    else if (mode == "capacity") capacity();
    else if (mode == "concurrent") concurrent();
    else if (mode == "metadata") metadata(path);
    else if (mode == "provenance-bind") provenance_bind();
    else if (mode == "provenance-final") provenance_final();
    else if (mode == "provenance-unreadable") provenance_unreadable();
    else if (mode == "provenance-retry") provenance_retry();
#endif
    else return 2;
    std::printf("assets.vfx-preflight %s: %d passed, %d failed\n", mode.c_str(), g_pass, g_fail);
    return g_fail ? 1 : 0;
}
