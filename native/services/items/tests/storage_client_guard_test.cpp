// SPDX-License-Identifier: MIT
// Host tests (ASan/UBSan) of the player storage client guard's setup (kiln#135): the reflected client input layout on
// both approved builds' real reflection (tests/fixtures/player-storage/client-input-types.json), what the guard refuses
// with it, and which live code at gameplay/DeliverEvents it chains onto, against kiln.hooks' own code-state registry.
#include "../storage_client_guard.h"
#include "fake_types.h"
#include "nlohmann/json.hpp"
#include "services/hooks_code.h"
#include "services/kiln_caps.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
using namespace kitems::storage;
using nlohmann::json;
int checks = 0, failed = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            ++failed;                                                                                                  \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                                                \
        }                                                                                                              \
    } while (0)
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void load(const json& build, fake_types::Catalog* c) {
    for (const auto& [name, row] : build.at("types").items()) {
        std::vector<fake_types::Field> fields;
        if (row.size() > 3)
            for (const auto& f : row[3])
                fields.push_back({f[0].get<std::string>(), f[1].get<std::string>(), f[2].get<uint64_t>()});
        c->add(name, row[0].get<uint32_t>(), row[1].get<uint8_t>(), row[2].get<uint32_t>(), std::move(fields));
    }
}

// Where the nine guarded actions are on both builds (docs/re/player-storage.md has the transfer action's numbers).
struct Expect {
    const char* name;
    uint32_t offset, size, version, consumed;
    std::vector<uint32_t> refs;
};
const Expect kExpect[]{
    {"inventoryTransferAction", 8, 32, 8, 32, {4, 8, 12, 20}},
    {"sortInventoryAction", 112, 8, 112, 36, {4}},
    {"playerCraftingAction", 188, 68, 248, 68, {0, 20}},
    {"areaDepositPermissionAction", 256, 12, 264, 72, {0}},
    {"upgradeStorageAction", 268, 60, 324, 76, {0}},
    {"changeEquipmentAction", 456, 16, 464, 120, {0}},
    {"swapEquipmentSetAction", 472, 12, 480, 124, {0}},
    {"itemAction", 484, 24, 500, 128, {0, 8}},
    {"weaponGemAction", 608, 28, 628, 152, {0, 4, 12}},
};

void layout_tests(const json& fixture) {
    CHECK(fixture.at("builds").size() == 2);
    for (const auto& build : fixture.at("builds")) {
        const std::string sha = build.at("exe_sha256").get<std::string>().substr(0, 12);
        std::printf("== build %s\n", sha.c_str());
        fake_types::Catalog c;
        load(build, &c);
        // The member that turned the guard off (kiln#135): an enum between the actions, whose field_count is its two
        // values, not fields.
        auto* group = c.find("keen::ecs::ClientPlayerInputData");
        auto* mode = c.find("keen::ecs::DirectionalInputMode");
        CHECK(group && mode && mode->kind == KILN_TYPE_ENUM && mode->count == 2);
        bool enum_member = false;
        for (const auto& f : group->fields)
            enum_member = enum_member || (f.name == "movementStickInputMode" && f.type == mode->qname);
        CHECK(enum_member);

        auto types = fake_types::api(&c);
        ClientLayout layout;
        const std::string why = client_layout(&types, &layout);
        CHECK(why.empty());
        if (!why.empty())
            std::printf("  client_layout: %s\n", why.c_str());
        CHECK(layout.input_size == 1472 && layout.consumed_size == 224);
        // the nine guarded actions plus the others with entity references (lore voiceover, teleport, dialog x2, flame
        // altar, logic entity config, redeem item)
        CHECK(layout.actions.size() == 16);
        for (const auto& e : kExpect) {
            const ClientActionLayout* a = nullptr;
            for (const auto& l : layout.actions)
                if (l.offset == e.offset)
                    a = &l;
            CHECK(a && a->size == e.size && a->version == e.version && a->consumed == e.consumed && a->refs == e.refs);
            if (!a || a->version != e.version || a->consumed != e.consumed || a->refs != e.refs)
                std::printf("  %s: not where it should be\n", e.name);
        }
        // The guard's property: an action that names a storage holder, page or item in any of its references is
        // refused; the same action naming only other entities (or none) is not.
        for (const auto& a : layout.actions) {
            for (size_t r = 0; r < a.refs.size(); ++r) {
                std::vector<uint8_t> payload(a.size);
                const uint32_t tracked = 4242, other = 77;
                for (auto off : a.refs)
                    std::memcpy(payload.data() + off, &other, 4);
                auto contains = [&](uint32_t e) { return e == tracked; };
                CHECK(!forbidden_action(payload.data(), a, contains));
                std::memcpy(payload.data() + a.refs[r], &tracked, 4);
                CHECK(forbidden_action(payload.data(), a, contains));
            }
            std::vector<uint8_t> empty(a.size);
            CHECK(!forbidden_action(empty.data(), a, [](uint32_t) { return true; }));
        }
        // Each refusal names what did not resolve, and leaves the layout alone.
        {
            fake_types::Catalog broken;
            load(build, &broken);
            auto& item = *broken.find("keen::ecs::ItemAction");
            for (auto& f : item.fields)
                if (f.name == "versionData")
                    f.name = "version";
            auto t = fake_types::api(&broken);
            ClientLayout untouched;
            untouched.input_size = 9;
            const auto w = client_layout(&t, &untouched);
            CHECK(has(w, "PlayerInput.fromClient.itemAction (keen::ecs::ItemAction) has entity references but no "
                         "versionData") &&
                  untouched.input_size == 9 && untouched.actions.empty());
        }
        {
            fake_types::Catalog broken;
            load(build, &broken);
            for (auto& f : broken.find("keen::ecs::ServerConsumedPlayerInput")->fields)
                if (f.name == "consumedWeaponGemAction")
                    f.name = "consumedChangeWeaponGemAction";
            auto t = fake_types::api(&broken);
            ClientLayout l;
            CHECK(client_layout(&t, &l) ==
                  "PlayerInput.fromClient.weaponGemAction: no consumedWeaponGemAction in ServerConsumedPlayerInput");
        }
        {
            fake_types::Catalog broken;
            load(build, &broken);
            for (auto& f : broken.find("keen::ecs::SortInventoryAction")->fields)
                if (f.name == "targetInventoryId")
                    f.type = "keen::ItemId"; // no entity reference left: not an action the guard can cover
            auto t = fake_types::api(&broken);
            ClientLayout l;
            CHECK(client_layout(&t, &l) == "PlayerInput.fromClient has no sortInventoryAction with entity references");
        }
        {
            fake_types::Catalog broken;
            load(build, &broken);
            broken.find("keen::InventorySlotId")->size = 12;
            auto t = fake_types::api(&broken);
            ClientLayout l;
            const auto w = client_layout(&t, &l);
            CHECK(has(w, "PlayerInput.fromClient.inventoryTransferAction.sourceSlotId (keen::InventorySlotId): 12 "
                         "bytes at +12"));
        }
        {
            fake_types::Catalog empty;
            auto t = fake_types::api(&empty);
            ClientLayout l;
            CHECK(client_layout(&t, &l) == "no PlayerInput type");
        }
    }
}

// ---- The live code at gameplay/DeliverEvents, classified by kiln.hooks' own registry ----
using namespace kiln::hooks::code;
constexpr uintptr_t kBase = 0x140000000, kText = kBase + 0x1000, kDeliver = kText + 0x40;
std::array<uint8_t, 0x200> file_bytes{}, live_bytes{};
bool live_readable = true;
bool file_copy(uintptr_t a, uint8_t* out, size_t n) {
    std::memcpy(out, file_bytes.data() + (a - kBase - 0x1000), n);
    return true;
}
bool live_copy(uintptr_t a, uint8_t* out, size_t n) {
    if (!live_readable)
        return false;
    std::memcpy(out, live_bytes.data() + (a - kBase - 0x1000), n);
    return true;
}
const Image kImage{kBase, 0x2000, kText, file_bytes.size(), file_copy, live_copy};
Registry* registry_now = nullptr;
KilnResult KILN_CALL code_state(uintptr_t at, size_t n, uint32_t* state, uint64_t* generation) {
    return registry_now->code_state(kImage, at, n, state, generation);
}
KilnResult KILN_CALL read_live(uintptr_t at, void* out, size_t n) {
    return live_copy(at, static_cast<uint8_t*>(out), n) ? KILN_OK : KILN_ERR_INVALID_ARGUMENT;
}
KilnResult KILN_CALL read_file(uintptr_t at, void* out, size_t n) {
    return read_original(kImage, at, out, n);
}
KilnHooksApi hooks_table() {
    KilnHooksApi h{};
    h.header = {sizeof h, 2};
    h.code_state = &code_state;
    h.read = &read_live;
    h.read_original = &read_file;
    return h;
}
// DeliverEvents' first 16 bytes on both approved builds (push rsi; push r13; push r14; sub rsp, 40h; ...).
constexpr uint8_t kPrologue[16]{0x40, 0x56, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83,
                                0xec, 0x40, 0x48, 0x8b, 0x31, 0x45, 0x33, 0xed};
// What a Kiln detour writes and records: a jmp rel32 over the first three instructions (6 bytes, one nop of padding).
std::vector<uint8_t> kiln_detour(uintptr_t to) {
    std::vector<uint8_t> b{0xe9, 0, 0, 0, 0, 0x90};
    const int32_t rel = static_cast<int32_t>(to - (kDeliver + 5));
    std::memcpy(b.data() + 1, &rel, 4);
    return b;
}
void record_kiln_detour(Registry& r, uintptr_t to) {
    const auto installed = kiln_detour(to);
    std::vector<uint8_t> original(live_bytes.begin() + 0x40, live_bytes.begin() + 0x40 + installed.size());
    auto guard = r.lock();
    r.add_locked(kDeliver, original, installed, true);
    std::memcpy(live_bytes.data() + 0x40, installed.data(), installed.size());
}
// What a module outside the registry writes (kiln-shroudlink's own detour: jmp [rip+0] with the address).
void foreign_jump(uintptr_t to) {
    const uint8_t b[6]{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(live_bytes.data() + 0x40, b, 6);
    std::memcpy(live_bytes.data() + 0x46, &to, 8);
}
void reset_code() {
    file_bytes.fill(0xcc);
    std::memcpy(file_bytes.data() + 0x40, kPrologue, sizeof kPrologue);
    live_bytes = file_bytes;
    live_readable = true;
}

void code_tests() {
    auto hooks = hooks_table();
    {
        // kiln-players raw-links DeliverEvents first, then storage asks; kiln-shroudlink hooks it later.
        Registry r;
        registry_now = &r;
        reset_code();
        CHECK(code_verdict(&hooks, kDeliver).ok); // the exe's bytes
        record_kiln_detour(r, kText + 0x180);
        uint32_t state = 0;
        uint64_t generation = 0;
        CHECK(code_state(kDeliver, 16, &state, &generation) == KILN_OK && state == KILN_CODE_MANAGED);
        const auto v = code_verdict(&hooks, kDeliver);
        CHECK(v.ok && v.why.empty()); // another Kiln owner's detour: the guard joins its chain
        foreign_jump(0x7ff812340000ull);
        const auto later = code_verdict(&hooks, kDeliver);
        CHECK(!later.ok && has(later.why, "outside Kiln's hook registry") && later.jump == 0x7ff812340000ull &&
              later.state == KILN_CODE_FOREIGN &&
              later.bytes == "live ff 25 00 00 00 00 00 00 34 12 f8 7f 00 00 33 ed, file 40 56 41 55 41 56 48 83 ec "
                             "40 48 8b 31 45 33 ed");
        std::printf("  foreign over Kiln's detour: %s\n", later.why.c_str());
    }
    {
        // Another module hooked DeliverEvents first and a Kiln detour went over it: still foreign, refused.
        Registry r;
        registry_now = &r;
        reset_code();
        foreign_jump(0x7ff812340000ull);
        CHECK(!code_verdict(&hooks, kDeliver).ok);
        record_kiln_detour(r, kText + 0x180);
        const auto v = code_verdict(&hooks, kDeliver);
        CHECK(!v.ok && has(v.why, "outside Kiln's hook registry") && v.jump == kText + 0x180);
    }
    {
        Registry r;
        registry_now = &r;
        reset_code();
        live_readable = false;
        const auto v = code_verdict(&hooks, kDeliver);
        CHECK(!v.ok && v.state == KILN_CODE_UNKNOWN && has(v.why, "could not be read") && !v.jump &&
              v.bytes == "file 40 56 41 55 41 56 48 83 ec 40 48 8b 31 45 33 ed");
        live_readable = true;
    }
    {
        auto old = hooks;
        old.header.size = offsetof(KilnHooksApi, code_state);
        CHECK(code_verdict(&old, kDeliver).why == "kiln.hooks cannot say who changed its code (no code_state)");
        CHECK(!code_verdict(nullptr, kDeliver).ok);
    }
    {
        // the jumps the verdict decodes
        auto none = [](uintptr_t, void*, size_t) { return false; };
        uint8_t b[16]{0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xff, 0xe0};
        CHECK(jump_target(0x1000, b, none) == 0x1122334455667788ull);
        const uint8_t e9[16]{0xe9, 0xfb, 0xff, 0xff, 0xff};
        CHECK(jump_target(0x1000, e9, none) == 0x1000);
        CHECK(jump_target(0x1000, kPrologue, none) == 0);
        const uint8_t eb[16]{0xeb, 0x10};
        CHECK(jump_target(0x1000, eb, none) == 0x1012);
        const uint8_t r11[16]{0x49, 0xbb, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x41, 0xff, 0xe3};
        CHECK(jump_target(0x1000, r11, none) == 0x1122334455667788ull);
    }
    registry_now = nullptr;
}

// ---- Where a foreign jump at DeliverEvents leads, and what the refusal tells the player ----
// A little address space: a few 16-byte cells of code or data, and the modules (name, base, size) loaded in it.
struct Memory {
    std::map<uintptr_t, std::array<uint8_t, 16>> cells;
    struct Module {
        std::string name;
        uintptr_t base;
        size_t size;
    };
    std::vector<Module> modules;
    void put(uintptr_t at, std::initializer_list<uint8_t> bytes, uint64_t tail = 0, size_t tail_at = 0) {
        std::array<uint8_t, 16> c{};
        std::copy(bytes.begin(), bytes.end(), c.begin());
        if (tail || tail_at)
            std::memcpy(c.data() + tail_at, &tail, 8);
        cells[at] = c;
    }
    bool read(uintptr_t at, void* out, size_t n) const {
        for (const auto& [base, c] : cells)
            if (at >= base && at - base + n <= c.size()) {
                std::memcpy(out, c.data() + (at - base), n);
                return true;
            }
        return false;
    }
    ModulePlace locate(uintptr_t at) const {
        for (const auto& m : modules)
            if (at >= m.base && at - m.base < m.size)
                return {m.name, at - m.base};
        return {};
    }
};
std::string hex16(const uint8_t* b) { return hex_bytes(b, 16); }

void refusal_tests() {
    // retour (shroudlink's own core, for one) on x64: jmp rel32 to a relay in private memory, the relay's jmp [rip+0]
    // to the hook in its DLL. The first hop is the relay (in no module); the chain ends in the DLL.
    Memory m;
    const uintptr_t deliver = 0x140a25a50, relay = 0x140001000, hook = 0x7ff812341234;
    m.modules = {{"enshrouded.exe", 0x140100000, 0x1000}, // not where the relay is
                 {"steam_api64.dll", 0x7ff812340000, 0x10000},
                 {"kiln-players.dll", 0x7ff900000000, 0x10000}};
    const int32_t rel = static_cast<int32_t>(relay - (deliver + 5));
    std::array<uint8_t, 16> entry{0xe9};
    std::memcpy(entry.data() + 1, &rel, 4);
    m.cells[deliver] = entry;
    m.put(relay, {0xff, 0x25, 0, 0, 0, 0}, hook, 6);
    auto read = [&](uintptr_t a, void* out, size_t n) { return m.read(a, out, n); };
    auto locate = [&](uintptr_t a) { return m.locate(a); };
    CHECK(jump_target(deliver, entry.data(), read) == relay);
    CHECK(follow_jumps(relay, read, locate) == hook);
    CHECK(follow_jumps(hook, read, locate) == hook); // already in a module
    // Kiln's own raw link stub: jmp [rip+disp32] through a cell that names the first link's handler.
    const uintptr_t stub = 0x140002000, cell = 0x140003000, handler = 0x7ff900001230;
    m.put(stub, {0xff, 0x25, 0xfa, 0x0f, 0, 0}); // [stub + 6 + 0xffa] = cell
    m.put(cell, {}, handler, 0);
    CHECK(follow_jumps(stub, read, locate) == handler);
    // a jump into memory that cannot be read, or a loop, ends where it is
    CHECK(follow_jumps(0x150000000, read, locate) == 0x150000000);
    m.put(0x140004000, {0xeb, 0xfe}); // jmp $
    CHECK(follow_jumps(0x140004000, read, locate) == 0x140004000);

    // The verdict kiln.hooks' registry gives for a foreign jmp [rip+0] at DeliverEvents (its words and bytes).
    Registry r;
    registry_now = &r;
    reset_code();
    foreign_jump(0x7ff812341234ull);
    auto hooks = hooks_table();
    const CodeVerdict v = code_verdict(&hooks, kDeliver);
    registry_now = nullptr;
    CHECK(!v.ok && v.state == KILN_CODE_FOREIGN && v.jump == 0x7ff812341234ull && !has(v.why, "live ") &&
          v.bytes == "live ff 25 00 00 00 00 34 12 34 12 f8 7f 00 00 33 ed, file 40 56 41 55 41 56 48 83 ec 40 48 8b 31 "
                     "45 33 ed");
    const std::string prefix = "items.storage: client inventory guard is unavailable: ";
    {
        // into another module: named, with what to do, and the bytes last
        const auto t = code_refusal("gameplay/DeliverEvents", v, hook, m.locate(hook));
        std::printf("  refusal: %s\n", t.c_str());
        CHECK(t.rfind("gameplay/DeliverEvents: " + v.why, 0) == 0 && has(t, "outside Kiln's hook registry"));
        CHECK(has(t, "its jump leads into steam_api64.dll+0x1234") && !has(t, "placed over another module's patch"));
        CHECK(has(t, "Remove that mod or loader") && has(t, "restart") && has(t, "kiln.log"));
        CHECK(has(t, "shroudlink's own core (steam_api64.dll, shroudlink_core.dll)"));
        CHECK(t.find("restart") < t.find("(live ff 25 00 00 00 00") && has(t, "file 40 56 41 55"));
        // what to do fits in a capability row's text (KILN_CAP_TEXT_MAX) behind storage_ready's prefix
        CHECK(prefix.size() + t.find(" (live ") <= KILN_CAP_TEXT_MAX);
    }
    {
        // Kiln's own detour sits in front (it was placed over the other module's patch)
        const auto t = code_refusal("gameplay/DeliverEvents", v, handler, m.locate(handler));
        std::printf("  refusal (Kiln's detour in front): %s\n", t.c_str());
        CHECK(has(t, "its jump leads into kiln-players.dll+0x1230 (Kiln's own detour, placed over another module's "
                     "patch)") &&
              has(t, "Remove that mod or loader"));
        CHECK(prefix.size() + t.find(" (live ") <= KILN_CAP_TEXT_MAX);
    }
    {
        // a relay no module owns, and another function than DeliverEvents: no shroudlink hint
        const auto t = code_refusal("characters/Capture", v, relay, {});
        CHECK(has(t, "its jump leads to 0x140001000 (in no module") && has(t, "Remove that mod or loader") &&
              !has(t, "shroudlink"));
    }
    {
        // no jump at all (bytes patched in place): no jump clause
        auto in_place = v;
        in_place.jump = 0;
        const auto t = code_refusal("gameplay/DeliverEvents", in_place, 0, {});
        CHECK(!has(t, "jump") && has(t, "Remove that mod or loader") && has(t, "(live ff 25"));
    }
    {
        // not foreign: the verdict's own words, no advice
        CodeVerdict unknown;
        unknown.state = KILN_CODE_UNKNOWN;
        unknown.why = "its live code could not be read";
        unknown.bytes = "live e9 00 00 00 00 90 48 83 ec 40 48 8b 31 45 33 ed, file " + hex16(kPrologue);
        const auto t = code_refusal("gameplay/DeliverEvents", unknown, 0, {});
        CHECK(t == "gameplay/DeliverEvents: its live code could not be read (live e9 00 00 00 00 90 48 83 ec 40 48 8b 31 "
                   "45 33 ed, file 40 56 41 55 41 56 48 83 ec 40 48 8b 31 45 33 ed)");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: storage-client-guard-test <client-input-types.json>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    const json fixture = json::parse(in);
    layout_tests(fixture);
    code_tests();
    refusal_tests();
    std::printf("storage client guard: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
