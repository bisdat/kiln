// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_hooks.h"
#include "services/kiln_types.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
namespace kitems::storage {
struct ClientActionLayout {
    uint32_t offset = 0, version = 0, consumed = 0, size = 0;
    std::vector<uint32_t> refs;
};
// Each ref is the u32 entity itself or the first u32 of an InventorySlotId. Callers validate reflected ranges once.
template <class Contains>
bool forbidden_action(const uint8_t* payload, const ClientActionLayout& action, Contains contains) {
    for (auto off : action.refs) {
        if (off > action.size || action.size - off < 4)
            return true;
        uint32_t entity = 0;
        std::memcpy(&entity, payload + off, 4);
        if (entity && contains(entity))
            return true;
    }
    return false;
}

// The client actions that address an inventory cell (docs/re/player-storage.md): the guard covers every one of them or
// is not installed.
inline constexpr const char* kGuardedClientActions[]{
    "inventoryTransferAction", "sortInventoryAction", "areaDepositPermissionAction",
    "upgradeStorageAction",    "changeEquipmentAction", "swapEquipmentSetAction",
    "itemAction",              "weaponGemAction",       "playerCraftingAction"};

struct ClientLayout {
    std::vector<ClientActionLayout> actions;
    uint32_t input_size = 0, consumed_size = 0;
};

namespace client_layout_detail {
inline std::string text(KilnStr s) { return s.ptr ? std::string(s.ptr, s.len) : std::string(); }
inline bool same(KilnStr s, const char* t) { return s.ptr && s.len == std::strlen(t) && !std::memcmp(s.ptr, t, s.len); }
inline bool info(const KilnTypesApi* types, const KilnType* t, KilnTypeInfo* out) {
    *out = {};
    out->size = sizeof *out;
    return t && types->type_info(t, out) == KILN_OK;
}
inline bool field(const KilnTypesApi* types, const KilnType* t, const char* name, KilnTypeField* out) {
    KilnTypeInfo i{};
    if (!info(types, t, &i) || i.kind != KILN_TYPE_STRUCT)
        return false;
    for (uint32_t n = 0; n < i.field_count; ++n)
        if (types->type_field(t, n, out) == KILN_OK && same(out->name, name))
            return true;
    return false;
}
} // namespace client_layout_detail

// PlayerInput.fromClient's actions from the game's reflection (kiln.types): for every member that is an action (a struct
// with versionData) and holds an entity or inventory slot, where its refs, its version and its consumed version in
// ServerConsumedPlayerInput are. Members that are not structs (an enum, a bitmask, a bool) are no actions: an enum's
// field_count counts its values, which are not fields. Returns "" with *out filled, or which member, type or field did
// not resolve (and *out is left alone).
inline std::string client_layout(const KilnTypesApi* types, ClientLayout* out) {
    using namespace client_layout_detail;
    if (!types || !types->type_by_name || !types->type_info || !types->type_field)
        return "kiln.types has no type lookup";
    const KilnType* pi = types->type_by_name({"PlayerInput", 11});
    const KilnType* scpi = types->type_by_name({"ServerConsumedPlayerInput", 25});
    KilnTypeInfo p{}, c{};
    if (!info(types, pi, &p))
        return "no PlayerInput type";
    if (!info(types, scpi, &c))
        return "no ServerConsumedPlayerInput type";
    if (p.type_size > 16384 || c.type_size > 16384)
        return "PlayerInput or ServerConsumedPlayerInput is larger than 16 KiB";
    KilnTypeField group{};
    KilnTypeInfo gi{};
    if (!field(types, pi, "fromClient", &group) || !info(types, group.type, &gi) || gi.kind != KILN_TYPE_STRUCT)
        return "PlayerInput has no fromClient struct";
    ClientLayout layout;
    layout.input_size = p.type_size;
    layout.consumed_size = c.type_size;
    std::set<std::string> guarded;
    for (uint32_t n = 0; n < gi.field_count; ++n) {
        KilnTypeField action{};
        if (types->type_field(group.type, n, &action) != KILN_OK)
            return "PlayerInput.fromClient member " + std::to_string(n) + " does not resolve";
        const std::string name = text(action.name), where = "PlayerInput.fromClient." + name;
        KilnTypeInfo ai{};
        if (!info(types, action.type, &ai))
            return where + ": its type does not resolve";
        if (ai.kind != KILN_TYPE_STRUCT)
            continue;
        const std::string type = text(ai.qualified_name);
        ClientActionLayout l{};
        l.offset = static_cast<uint32_t>(group.offset + action.offset);
        l.size = ai.type_size;
        for (uint32_t k = 0; k < ai.field_count; ++k) {
            KilnTypeField f{};
            KilnTypeInfo fi{};
            if (types->type_field(action.type, k, &f) != KILN_OK)
                return where + " (" + type + "): field " + std::to_string(k) + " does not resolve";
            if (!info(types, f.type, &fi))
                return where + "." + text(f.name) + ": its type does not resolve";
            if (same(fi.qualified_name, "keen::ecs::EntityId") || same(fi.qualified_name, "keen::InventorySlotId")) {
                if (f.offset > l.size || fi.type_size > l.size - f.offset || (fi.type_size != 4 && fi.type_size != 8))
                    return where + "." + text(f.name) + " (" + text(fi.qualified_name) + "): " +
                           std::to_string(fi.type_size) + " bytes at +" + std::to_string(f.offset) +
                           " does not fit an entity or slot reference";
                l.refs.push_back(static_cast<uint32_t>(f.offset));
            }
        }
        if (l.refs.empty())
            continue;
        KilnTypeField version{}, used{};
        KilnTypeInfo vi{}, ui{};
        if (!field(types, action.type, "versionData", &version))
            return where + " (" + type + ") has entity references but no versionData";
        if (!info(types, version.type, &vi) || vi.type_size != 4 || version.offset > l.size ||
            4 > l.size - version.offset)
            return where + ".versionData is not 4 bytes inside the action";
        l.version = l.offset + static_cast<uint32_t>(version.offset);
        std::string consumed = "consumed" + name;
        if (consumed.size() > 8 && consumed[8] >= 'a' && consumed[8] <= 'z')
            consumed[8] = static_cast<char>(consumed[8] - 32);
        if (!field(types, scpi, consumed.c_str(), &used))
            return where + ": no " + consumed + " in ServerConsumedPlayerInput";
        if (!info(types, used.type, &ui) || ui.type_size != 4 || used.offset > c.type_size ||
            4 > c.type_size - used.offset)
            return "ServerConsumedPlayerInput." + consumed + " is not 4 bytes inside the struct";
        if (l.offset > p.type_size || l.size > p.type_size - l.offset || l.size > 160)
            return where + " (" + type + ", " + std::to_string(l.size) + " bytes at +" + std::to_string(l.offset) +
                   ") is not inside PlayerInput or is larger than 160 bytes";
        l.consumed = static_cast<uint32_t>(used.offset);
        layout.actions.push_back(l);
        guarded.insert(name);
    }
    for (const char* name : kGuardedClientActions)
        if (!guarded.count(name))
            return std::string("PlayerInput.fromClient has no ") + name + " with entity references";
    *out = std::move(layout);
    return {};
}

// The live code at a target Kiln wants to chain a hook on. Accepted: the exe file's bytes, or bytes only Kiln's hook
// registry changed (another Kiln owner's detour: the new link joins that chain). Never accepted: another module's hook
// or patch, before or over Kiln's detour, or bytes Kiln cannot read or account for.
struct CodeVerdict {
    bool ok = false;
    uint32_t state = KILN_CODE_UNKNOWN; // what kiln.hooks' code_state said (KILN_CODE_*)
    std::string why;                    // when !ok: what kiln.hooks said
    std::string bytes;                  // when !ok: "live <16 bytes>, file <16 bytes>", as far as they could be read
    uintptr_t jump = 0;                 // when foreign and the live bytes start with a jump: its first hop
};
inline std::string hex_bytes(const uint8_t* b, size_t n) {
    std::string s;
    char part[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(part, sizeof part, i ? " %02x" : "%02x", b[i]);
        s += part;
    }
    return s;
}
// Where the jump at the start of `b` (16 bytes at `at`) goes: jmp rel32 or rel8, call rel32, jmp [rip+disp32] (the
// address read through `read(address, out, 8)`), mov rax/r11, imm64 + jmp. 0 when the bytes start with none of them.
// The forms kiln.hooks' own analysis follows (hooks_chain.cpp first_hop).
template <class Read> uintptr_t jump_target(uintptr_t at, const uint8_t* b, Read read) {
    int32_t rel = 0;
    uint64_t to = 0;
    if (b[0] == 0xe9 || b[0] == 0xe8) {
        std::memcpy(&rel, b + 1, 4);
        return at + 5 + static_cast<intptr_t>(rel);
    }
    if (b[0] == 0xeb)
        return at + 2 + static_cast<int8_t>(b[1]);
    if (b[0] == 0xff && b[1] == 0x25) {
        std::memcpy(&rel, b + 2, 4);
        return read(at + 6 + static_cast<intptr_t>(rel), &to, 8) ? static_cast<uintptr_t>(to) : 0;
    }
    if (b[0] == 0x48 && b[1] == 0xb8 && b[10] == 0xff && b[11] == 0xe0) {
        std::memcpy(&to, b + 2, 8);
        return static_cast<uintptr_t>(to);
    }
    if (b[0] == 0x49 && b[1] == 0xbb && b[10] == 0x41 && b[11] == 0xff && b[12] == 0xe3) {
        std::memcpy(&to, b + 2, 8);
        return static_cast<uintptr_t>(to);
    }
    return 0;
}
// A loaded module and the offset of an address in it; an empty name when no module holds the address.
struct ModulePlace {
    std::string name;
    uint64_t offset = 0;
};
// Where a chain of jumps that reached `to` ends: the first address in a module (`locate(address).name` not empty), or
// the last one whose bytes cannot be read or are no jump (a relay or stub of its own), after at most `hops` more jumps.
// A hook's relay (retour's jmp [rip+0] in private memory, Kiln's own stub) is followed into the module behind it.
template <class Read, class Locate>
uintptr_t follow_jumps(uintptr_t to, Read read, Locate locate, int hops = 5) {
    for (int i = 0; to && i < hops && locate(to).name.empty(); ++i) {
        uint8_t b[16]{};
        if (!read(to, b, sizeof b))
            break;
        const uintptr_t next = jump_target(to, b, read);
        if (!next || next == to)
            break;
        to = next;
    }
    return to;
}
inline CodeVerdict code_verdict(const KilnHooksApi* hooks, uintptr_t at) {
    constexpr size_t n = 16;
    CodeVerdict v;
    if (!hooks || hooks->header.size < offsetof(KilnHooksApi, code_state) + sizeof(hooks->code_state) ||
        !hooks->code_state) {
        v.why = "kiln.hooks cannot say who changed its code (no code_state)";
        return v;
    }
    uint32_t state = KILN_CODE_UNKNOWN;
    uint64_t generation = 0;
    const KilnResult rc = hooks->code_state(at, n, &state, &generation);
    if (rc != KILN_OK) {
        v.why = "kiln.hooks code_state refused its first " + std::to_string(n) + " bytes (result " +
                std::to_string(rc) + ")";
        return v;
    }
    v.state = state;
    if (state == KILN_CODE_ORIGINAL || state == KILN_CODE_MANAGED) {
        v.ok = true;
        return v;
    }
    auto read = [hooks](uintptr_t a, void* out, size_t size) {
        return hooks->read && hooks->read(a, out, size) == KILN_OK;
    };
    uint8_t live[n]{}, file[n]{};
    const bool have_live = read(at, live, n);
    const bool have_file = hooks->read_original && hooks->read_original(at, file, n) == KILN_OK;
    if (have_live)
        v.bytes += "live " + hex_bytes(live, n);
    if (have_file)
        v.bytes += std::string(v.bytes.empty() ? "" : ", ") + "file " + hex_bytes(file, n);
    if (state == KILN_CODE_FOREIGN) {
        v.why = "its code was hooked or patched outside Kiln's hook registry (by another module, before or over Kiln's "
                "own detour), which storage does not build on";
        if (have_live)
            v.jump = jump_target(at, live, read);
    } else if (state == KILN_CODE_UNKNOWN)
        v.why = "its live code could not be read, changed while it was read, or does not lead back to the exe's "
                "bytes through Kiln's own detours";
    else
        v.why = "kiln.hooks code_state answered " + std::to_string(state);
    return v;
}
// A Kiln module's file name: kiln-core.dll, kiln-<service or plugin>.dll (kiln.hooks' own rule for "a Kiln module").
inline bool kiln_module(const std::string& file) {
    std::string low = file;
    for (char& c : low)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return low.size() > 9 && low.rfind("kiln-", 0) == 0 && low.compare(low.size() - 4, 4, ".dll") == 0;
}
// What the capability text says when code_verdict refused `name`: the function and why; for another module's hook or
// patch where its jump leads (`to`, in `where` when a module holds it) and what the player can do; the bytes last, so a
// capability row's 512 bytes keep the rest. Module file names only, never a path.
inline std::string code_refusal(const char* name, const CodeVerdict& v, uintptr_t to, const ModulePlace& where) {
    std::string t = std::string(name) + ": " + v.why;
    if (v.state == KILN_CODE_FOREIGN) {
        char at[48];
        if (v.jump && !where.name.empty()) {
            std::snprintf(at, sizeof at, "+0x%llx", static_cast<unsigned long long>(where.offset));
            t += "; its jump leads into " + where.name + at;
            if (kiln_module(where.name))
                t += " (Kiln's own detour, placed over another module's patch)";
        } else if (v.jump) {
            std::snprintf(at, sizeof at, "0x%llx", static_cast<unsigned long long>(to ? to : v.jump));
            t += std::string("; its jump leads to ") + at + " (in no module: a relay or stub)";
        }
        t += ". Remove that mod or loader and restart (kiln.log's hooks lines name it)";
        if (!std::strcmp(name, "gameplay/DeliverEvents"))
            t += "; shroudlink's own core (steam_api64.dll, shroudlink_core.dll) is one: Kiln runs shroudlink mods "
                 "without it";
    }
    if (!v.bytes.empty())
        t += " (" + v.bytes + ")";
    return t;
}
} // namespace kitems::storage
