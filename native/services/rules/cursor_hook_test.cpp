// SPDX-License-Identifier: MIT
// Execute the production build-cursor waiver hook (cursor.cpp) without a game: where it patches, that a native mod that
// finds rules/CursorFlags by its first 42 bytes (Building Tools 0.34.0, which then redirects the game's calls to the
// entry) still finds it after Kiln's hook is in, and what the hook clears.
#define KILN_RULES_HOST_TEST
#include "rules.cpp"
#include "cursor.cpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace kiln_rules_creative {
bool idle() { return false; }
bool flame_creative() { return false; }
}  // namespace kiln_rules_creative

// rules.cpp's part, faked: a cursor context's first qword points at a program context whose first u32 is the entity;
// entity 7 has `rule`, others none. `throws` stands in for a fault reading the game's memory.
KilnRulesPlayer rule{};
bool throws = false;
int lookups = 0;
bool kiln_rules_bridge::cursor_rules(void* cctx, KilnRulesPlayer* out) {
    ++lookups;
    if (throws) throw 1;
    const auto* entity = *static_cast<uint32_t* const*>(cctx);
    if (!entity || *entity != 7) return false;
    *out = rule;
    return true;
}

namespace {
int failures = 0;
void check(bool ok, const std::string& why) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", why.c_str()); failures += !ok; }

// rules/CursorFlags as sigdb/symbols/server.d/rules.json finds it (its pattern, holes as 0x90) and the stretch
// rules/CursorFlagsStored's pattern covers, at their offsets in both executables (rules_sigdb_test.py checks those on
// the real files). The rest of the function is 0xCC.
constexpr const char* kEntry = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D 6C 24 C0 48 81 EC 40 01 00 00 "
                               "0F B6 85 88 00 00 00 4D 8B F1 88 44 24 28 49 8B F8 0F B6 85 80 00 00 00 4D 8B C8 4D 8B C6 "
                               "88 44 24 20 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8E ?? ?? ?? ?? B2 07 E8 ?? ?? ?? ?? 45 33 FF 84 C0";
constexpr const char* kStored = "48 8B D7 48 8B CE E8 ?? ?? ?? ?? 8B 13 8B CA 0F BA F1 0C 0F BA EA 0C 84 C0 0F 44 D1 89 13 "
                                "44 38 7F 54 0F 85 ?? ?? ?? ??";
constexpr size_t kStoredAt = 0xdc, kSiteAt = 0xfa;  // the pattern's start and rules/CursorFlagsStored (its offset 30)
// Building Tools 0.34.0 (src/main.cpp SIG_CURSOR_FLAGS): the function's first 42 bytes, exactly; its MissingCall
// signature (SIG_MISSING_CALL) is the material check's call and the store after it, entry + 0xe2 .. + 0xfa.
constexpr size_t kBtEntryBytes = 42, kBtMissingFrom = 0xe2, kBtMissingTo = 0xfa;

std::vector<uint8_t> bytes_of(const char* pattern) {
    std::vector<uint8_t> out;
    for (const char* p = pattern; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        out.push_back(p[0] == '?' ? 0x90 : static_cast<uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    return out;
}

alignas(16) uint8_t image[0x1000];
constexpr size_t kFn = 0x400;  // rules/CursorFlags in the fake image
uintptr_t fn_at() { return reinterpret_cast<uintptr_t>(image + kFn); }
const uint8_t* begin() { return image; }
const uint8_t* end() { return image + sizeof image; }

void build_image() {
    std::memset(image, 0xcc, sizeof image);
    const auto entry = bytes_of(kEntry), stored = bytes_of(kStored);
    std::memcpy(image + kFn, entry.data(), entry.size());
    std::memcpy(image + kFn + kStoredAt, stored.data(), stored.size());
    // the game's four calls to it (the two cursor programs, primary and secondary): Building Tools redirects these
    for (size_t at : {size_t{0x40}, size_t{0x80}, size_t{0x900}, size_t{0x980}}) {
        image[at] = 0xe8;
        const int32_t rel = static_cast<int32_t>(fn_at() - reinterpret_cast<uintptr_t>(image + at + 5));
        std::memcpy(image + at + 1, &rel, 4);
    }
}

// What kiln.hooks writes at a target (safetyhook's 5-byte jump, the rest of the instructions it moves filled with nop)
void patch(uintptr_t target, size_t moved) {
    auto* p = reinterpret_cast<uint8_t*>(target);
    p[0] = 0xe9;
    std::memset(p + 1, 0x11, 4);
    std::memset(p + 5, 0x90, moved - 5);
}

// Building Tools' install_overlap: find_unique (one exact match of the signature in the code), then hook_calls (every
// E8 that reaches it is redirected; none: "not hooked").
struct BtResult { int matches = 0; const uint8_t* at = nullptr; int calls = 0; };
BtResult bt_scan(const std::vector<uint8_t>& sig) {
    BtResult r;
    for (const uint8_t* p = begin(); p + sig.size() <= end(); ++p)
        if (std::memcmp(p, sig.data(), sig.size()) == 0) ++r.matches, r.at = p;
    if (r.matches != 1) return r;
    for (const uint8_t* p = begin(); p + 5 <= end(); ++p) {
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, 4);
        if (p[0] == 0xe8 && p + 5 + rel == r.at) ++r.calls;
    }
    return r;
}

std::vector<std::string> logs;
void KILN_CALL log(KilnOwner, int32_t, KilnStr text) { logs.emplace_back(text.ptr, text.len); }
bool have_stored = true;
KilnResult KILN_CALL symbol(KilnStr name, uintptr_t* out) {
    const std::string n(name.ptr, name.len);
    *out = n == "rules/CursorFlags" ? fn_at() : n == "rules/CursorFlagsStored" && have_stored ? fn_at() + kSiteAt : 0;
    return *out ? KILN_OK : KILN_ERR_NOT_FOUND;
}
std::vector<uintptr_t> installed;
void on_install(uintptr_t target) { installed.push_back(target); }
}  // namespace

int main() {
    KilnApi api{};
    api.symbol = &symbol;
    api.log = &log;
    g_api = &api;
    kiln::test::on_install = &on_install;
    build_image();
    const std::vector<uint8_t> entry = bytes_of(kEntry);
    const std::vector<uint8_t> bt_entry(entry.begin(), entry.begin() + kBtEntryBytes);
    const std::vector<uint8_t> missing(image + kFn + kBtMissingFrom, image + kFn + kBtMissingTo);
    {
        const BtResult before = bt_scan(bt_entry);
        check(before.matches == 1 && before.at == image + kFn && before.calls == 4,
              "the fake game: Building Tools finds the entry by its first 42 bytes and its 4 calls");
    }

    // GitHub/Discord (XHL, 2026-11-25): Kiln hooked the entry at load, 4 s before Building Tools scans, so the signature
    // missed and its overlap objects stayed off. The hook goes where the game stores the flags, below both of Building
    // Tools' signatures, and the scan finds the function in either order.
    check(kiln_rules_cursor::install(&api, 1), "the cursor waivers' hook is installed");
    check(installed.size() == 1, "one hook, one target");
    const uintptr_t target = installed.empty() ? 0 : installed[0];
    check(target == fn_at() + kSiteAt, "it is rules/CursorFlagsStored (entry + 0xfa), not the entry");
    check(target >= fn_at() + kBtEntryBytes && target >= fn_at() + kBtMissingTo,
          "below Building Tools' 42-byte entry signature and the end of its MissingCall signature");
    if (target) patch(target, 10);  // cmp byte [rdi+0x54], r15b (4) + jne rel32 (6)
    {
        const BtResult after = bt_scan(bt_entry);
        check(after.matches == 1 && after.at == image + kFn,
              "Building Tools started after Kiln: CursorFlags found (overlap objects available)");
        check(after.calls == 4, "... and the four calls still reach the entry, so its redirect has calls to take");
        check(std::memcmp(image + kFn + kBtMissingFrom, missing.data(), missing.size()) == 0,
              "... and its MissingCall signature's bytes are untouched");
    }

    // What the hook clears: the bits the player's rules waive, in the stored flags, nothing else.
    uint32_t entity = 7;
    uint32_t* program = &entity;
    void* cctx = &program;
    safetyhook::Context c{};
    uint32_t flags = 0;
    c.rbx = reinterpret_cast<uintptr_t>(&flags);
    c.rsi = reinterpret_cast<uintptr_t>(cctx);
    using namespace kiln_rules_cursor;
    const uint32_t all = kOutsideZone | kInFog | kLenientInhibitor | kStrictInhibitor | kEnemies | kMissingMaterials |
                         kNoPermission | (1u << 8) | 1u;
    rule = kDefault;
    flags = all;
    hk_stored(c);
    check(flags == all && cleared() == 0, "default rules: the game's flags stand");
    rule.flags = KILN_RULES_BUILD_OUTSIDE_BASES | KILN_RULES_IGNORE_ENEMIES;
    hk_stored(c);
    check(flags == (all & ~(kOutsideZone | kEnemies)) && cleared() == 1, "BuildOutsideBases and IgnoreEnemiesNearby clear their bits");
    rule.flags = KILN_RULES_IGNORE_NO_BUILD_ZONES | KILN_RULES_IGNORE_SHROUD | KILN_RULES_IGNORE_BASE_PERMISSIONS;
    rule.build_cost = 0.0f;
    flags = all;
    hk_stored(c);
    check(flags == (1u | (1u << 8) | kOutsideZone | kEnemies),
          "no-build zones, the Shroud, base permissions and free building clear theirs; invalid transform and AreaOccupied stay");
    entity = 9;
    flags = all;
    hk_stored(c);
    check(flags == all, "another player's cursor keeps its flags");
    entity = 7;
    flags = 0;
    const uint64_t before = cleared();
    hk_stored(c);
    check(flags == 0 && cleared() == before, "nothing to clear: not counted");
    throws = true;
    flags = all;
    hk_stored(c);
    check(flags == all && faults() == 1, "a fault reading the game's memory counts, and the game's flags stand");
    throws = false;
    c.rbx = 0;
    const int seen = lookups;
    hk_stored(c);
    check(lookups == seen && faults() == 1, "no flags pointer: nothing read");

    // When the site is not there (another build) or kiln.hooks refuses it: off, and said so; never the entry instead.
    installed.clear();
    have_stored = false;
    check(!kiln_rules_cursor::install(&api, 1) && installed.empty(), "no rules/CursorFlagsStored on this build: no hook at all");
    have_stored = true;
    kiln::test::fail_all = true;
    logs.clear();
    check(!kiln_rules_cursor::install(&api, 1) && installed.empty(), "a refused hook: off");
    check(!logs.empty() && logs.back().find("rules/CursorFlagsStored") != std::string::npos, "... with a kiln.log line naming the site");
    kiln::test::fail_all = false;
    std::printf("cursor hook: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
