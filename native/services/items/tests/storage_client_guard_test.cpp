// SPDX-License-Identifier: MIT
// Host tests (ASan/UBSan) of the player storage client guard's setup (kiln#135): the reflected client input layout on
// both approved builds' real reflection (tests/fixtures/player-storage/client-input-types.json), what the guard refuses
// with it, and which live code at gameplay/DeliverEvents it chains onto, against kiln.hooks' own code-state registry.
#include "../storage_client_guard.h"
#include "fake_types.h"
#include "nlohmann/json.hpp"
#include "services/hooks_code.h"
#include <array>
#include <cstdio>
#include <fstream>
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
              has(later.why, "live ff 25 00 00 00 00 00 00 34 12 f8 7f 00 00 33 ed") &&
              has(later.why, "file 40 56 41 55 41 56 48 83 ec 40 48 8b 31 45 33 ed"));
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
        CHECK(!v.ok && has(v.why, "could not be read") && has(v.why, "(file 40 56") && !has(v.why, "live 40"));
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
        uint8_t b[16]{0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xff, 0xe0};
        CHECK(jump_target(0x1000, b) == 0x1122334455667788ull);
        const uint8_t e9[16]{0xe9, 0xfb, 0xff, 0xff, 0xff};
        CHECK(jump_target(0x1000, e9) == 0x1000);
        CHECK(jump_target(0x1000, kPrologue) == 0);
    }
    registry_now = nullptr;
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
    std::printf("storage client guard: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
