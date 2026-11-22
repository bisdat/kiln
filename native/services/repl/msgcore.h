// SPDX-License-Identifier: MIT
// kiln.repl.net's message hooks and the game's message codec: the parts that are plain logic (no game call, no hook),
// shared by schema.cpp and messages.cpp and tested by tests/repl_msg_test.cpp. Engine facts: docs/re/network.md
// "Schema and receive sites"; the static read of SchemaEncode / SchemaDecode (server 0x9a8180 / 0x9a77e0, client
// 0x11f6420 / 0x11f5be0) in this package's report.
//
// The game's message schema (.data records 0x28 bytes apart): {const char* name; u32 size; u32 flags; const Field*
// fields; u64 count; u64 0}. Field, 0x40 bytes: +0 name, +8 u32 type (how it is coded on the wire), +0xc u32 element
// (how one element is stored in the message struct; the game's element-size table), +0x10 u32 shape, +0x18 u64 offset,
// +0x20 u64 present-flag (shape 0) or u32 count (shape 2) offset, +0x28 u64 fixed count, +0x30 u64 bits (types 13, 15),
// +0x38 nested (type 12: a schema; type 13: an enum descriptor {const char* name; u64 size; ...}).
// Wire types 0..16 (both coders): 0 bool (1 bit), 1 float (32 raw bits), 2 double: no case (the game's coder stops the
// process), 3/4 s8/u8, 5/6 s16/u16, 7/8 s32/u32, 9/10 s64/u64 (high word first), 11 a NUL-terminated char[fixed] (at most
// fixed-1 chars; count 1), 12 a nested struct, 13 an enum (bits from +0x30, stored by the enum's size; the encoder reads a
// u32), 14 a 16-byte GUID, 15 an unsigned field of +0x30 bits stored as element 4/6/8/10, 16 a u32 varint (7 bits a
// group, a continue bit first); 17+: no case. Shapes: 0 optional (a present bit; the encoder writes 'fixed' elements, the
// decoder reads one), 1 inline ('fixed' elements; 1 for a string), 2 dynamic (a u32 count in the struct, 16 bits on the
// wire, the decoder refuses a count over 'fixed'; the encoder trusts it).
// Elements (+0xc): 0 bool 1, 1 float 4, 2 double 8, 3/4 s8/u8, 5/6 s16/u16, 7/8 s32/u32, 9/10 s64/u64, 11 struct and
// 12 enum (the nested record's qword at +8), 13 GUID 16, 14 char 1.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "services/kiln_hooks.h"
#include "services/kiln_repl.h"

namespace repl::msg {

constexpr size_t kSchemaName = 0x00, kSchemaSize = 0x08, kSchemaFlags = 0x0c, kSchemaFields = 0x10, kSchemaCount = 0x18;
constexpr size_t kFieldStride = 0x40, kFieldName = 0x00, kFieldType = 0x08, kFieldElement = 0x0c, kFieldShape = 0x10,
                 kFieldOffset = 0x18, kFieldCountOff = 0x20, kFieldFixed = 0x28, kFieldBits = 0x30, kFieldNested = 0x38;

enum : uint32_t {
    Bool = 0, Float = 1, Double = 2, S8 = 3, U8 = 4, S16 = 5, U16 = 6, S32 = 7, U32 = 8, S64 = 9, U64 = 10, String = 11,
    Struct = 12, Enum = 13, Guid = 14, Bits = 15, VarU32 = 16,
};
enum : uint32_t { Optional = 0, Scalar = 1, Dynamic = 2 };
enum : uint32_t {
    EBool = 0, EFloat = 1, EDouble = 2, ES8 = 3, EU8 = 4, ES16 = 5, EU16 = 6, ES32 = 7, EU32 = 8, ES64 = 9, EU64 = 10,
    EStruct = 11, EEnum = 12, EGuid = 13, EChar = 14,
};
constexpr uint32_t kMaxDepth = 8, kMaxFields = 256, kMaxSchemaSize = 0x100000, kMaxDynamic = 0xffff;
constexpr uint64_t kMaxMessage = 0x4000;  // the session's largest message (the "big" flag)

template <class T>
inline T at(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

// One element's bytes in the message struct (the game's own table); 0 = unknown.
inline uint64_t element_size(uint32_t element, const uint8_t* nested) {
    switch (element) {
    case EBool: case ES8: case EU8: case EChar: return 1;
    case ES16: case EU16: return 2;
    case EFloat: case ES32: case EU32: return 4;
    case EDouble: case ES64: case EU64: return 8;
    case EGuid: return 16;
    case EStruct: case EEnum: return nested ? at<uint64_t>(nested, kSchemaSize) : 0;  // the game reads a qword
    default: return 0;
    }
}

namespace detail {
inline void say(char* why, size_t cap, const uint8_t* field, const char* what) {
    if (!why || !cap) return;
    char name[48] = "?";
    if (field) {
        const char* p = at<const char*>(field, kFieldName);
        size_t k = 0;
        while (p && k + 1 < sizeof name && p[k]) name[k] = p[k], ++k;
        if (p) name[k] = 0;
        std::snprintf(why, cap, "field %s: %s", name, what);
    } else {
        std::snprintf(why, cap, "%s", what);
    }
}
}  // namespace detail

using OnSchema = void (*)(const uint8_t* schema, void* user);

// Whether Kiln may hand schema s (and every schema it nests) to the game's coders: every type, shape and element known
// and consistent, every field inside the struct. A schema that fails is refused, never guessed. 'on' sees each schema
// checked (the nested ones too). Reads the records directly: in the game, call it inside a guard.
inline bool validate(const uint8_t* s, char* why, size_t cap, uint32_t depth = 0, OnSchema on = nullptr, void* user = nullptr) {
    auto fail = [&](const uint8_t* f, const char* what) {
        detail::say(why, cap, f, what);
        return false;
    };
    if (!s) return fail(nullptr, "no schema");
    if (depth > kMaxDepth) return fail(nullptr, "nested deeper than 8");
    const uint32_t size = at<uint32_t>(s, kSchemaSize), flags = at<uint32_t>(s, kSchemaFlags);
    const uint8_t* fields = at<const uint8_t*>(s, kSchemaFields);
    const uint64_t count = at<uint64_t>(s, kSchemaCount);
    if (!size || size > kMaxSchemaSize) return fail(nullptr, "size out of range");
    if (flags) return fail(nullptr, "flags not 0");
    if (!fields || !count || count > kMaxFields) return fail(nullptr, "no fields, or more than 256");
    if (on) on(s, user);
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* f = fields + i * kFieldStride;
        const uint32_t type = at<uint32_t>(f, kFieldType), el = at<uint32_t>(f, kFieldElement), shape = at<uint32_t>(f, kFieldShape);
        const uint64_t off = at<uint64_t>(f, kFieldOffset), coff = at<uint64_t>(f, kFieldCountOff);
        const uint64_t fixed = at<uint64_t>(f, kFieldFixed), bits = at<uint64_t>(f, kFieldBits);
        const uint8_t* nested = at<const uint8_t*>(f, kFieldNested);
        if (type > VarU32 || type == Double) return fail(f, "a type Kiln does not know");
        if (shape > Dynamic) return fail(f, "a shape Kiln does not know");
        if (fixed > kMaxSchemaSize) return fail(f, "fixed count out of range");
        const uint64_t esize = element_size(el, (el == EStruct || el == EEnum) ? nested : nullptr);
        uint64_t want = 0;  // the coder's bytes per element (0: checked in its case)
        switch (type) {
        case Bool: case S8: case U8: case String: want = 1; break;
        case S16: case U16: want = 2; break;
        case Float: case S32: case U32: case VarU32: want = 4; break;
        case S64: case U64: want = 8; break;
        case Guid: want = 16; break;
        case Struct:
            if (el != EStruct || !nested) return fail(f, "a struct without its schema");
            if (!validate(nested, why, cap, depth + 1, on, user)) return false;
            break;
        case Enum:
            if (el != EEnum || !nested) return fail(f, "an enum without its descriptor");
            if (esize != 1 && esize != 2 && esize != 4 && esize != 8) return fail(f, "an enum of an odd size");
            if (bits < 1 || bits > 32) return fail(f, "enum bits out of range");
            break;
        case Bits:
            if (el != EU8 && el != EU16 && el != EU32 && el != EU64) return fail(f, "bits of a storage Kiln does not know");
            if (bits < 1 || bits > esize * 8) return fail(f, "more bits than the storage holds");
            break;
        default: break;
        }
        if (want && esize != want) return fail(f, "the storage does not fit the coding");
        if (!esize) return fail(f, "an element of unknown size");
        uint64_t bytes = esize * fixed;
        if (type == String) {
            if (shape == Dynamic) return fail(f, "a string with a count");
            if (!fixed) return fail(f, "a string without room");
            bytes = fixed;
        }
        if (shape == Optional) {
            if (type != String && fixed != 1) return fail(f, "an optional of more than one element");
            if (coff >= size) return fail(f, "present flag past the struct");
        }
        if (shape == Dynamic) {
            if (fixed > kMaxDynamic) return fail(f, "capacity over the wire's 16-bit count");
            if (coff + 4 > size) return fail(f, "count past the struct");
        }
        if (off > size || bytes > size - off) return fail(f, "storage past the struct");
    }
    return true;
}

// Before an encode (the game's encoder trusts the counts): every dynamic count of msg, and of the structs in use inside
// it, within its field's capacity. s passed validate().
inline bool counts_ok(const uint8_t* s, const uint8_t* msg, uint32_t depth = 0) {
    if (depth > kMaxDepth) return false;
    const uint8_t* fields = at<const uint8_t*>(s, kSchemaFields);
    const uint64_t count = at<uint64_t>(s, kSchemaCount);
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* f = fields + i * kFieldStride;
        const uint32_t type = at<uint32_t>(f, kFieldType), shape = at<uint32_t>(f, kFieldShape);
        const uint64_t off = at<uint64_t>(f, kFieldOffset), coff = at<uint64_t>(f, kFieldCountOff), fixed = at<uint64_t>(f, kFieldFixed);
        uint64_t n = fixed;
        if (shape == Dynamic) {
            const uint32_t c = at<uint32_t>(msg, coff);
            if (c > fixed) return false;
            n = c;
        } else if (shape == Optional) {
            n = msg[coff] ? fixed : 0;
        }
        if (type == Struct) {
            const uint8_t* nested = at<const uint8_t*>(f, kFieldNested);
            const uint64_t esize = element_size(EStruct, nested);
            for (uint64_t k = 0; k < n; ++k)
                if (!counts_ok(nested, msg + off + k * esize, depth + 1)) return false;
        }
    }
    return true;
}

// ---- channels (the dossier's chapter 15 §4 M4) ----
// Hooks: every game channel but 4 and 5 (entity level: the filters), 12 (Kiln's own), 14 (no socket), 17 (read elsewhere).
inline bool hookable(uint32_t ch) { return ch < 25 && ch != 4 && ch != 5 && ch != 12 && ch != 14 && ch != 17; }
// Inject: the schema channels only (valid game messages by construction).
inline bool injectable(uint32_t ch) { return ch == 1 || ch == 15 || ch == 19 || ch == 20 || ch == 21; }
constexpr uint8_t kPlayerSocket = 1, kMachineSocket = 2;  // the socket's kind byte (+0) as the session keeps it
// The socket the client's receive loop reads a channel from; 0: none.
inline uint8_t inject_socket(uint32_t ch) {
    switch (ch) {
    case 0: case 1: case 2: case 3: case 15: case 16: case 18: case 19: case 20: case 22: return kMachineSocket;
    case 7: case 9: case 10: case 11: case 21: case 23: case 24: return kPlayerSocket;
    default: return 0;
    }
}

// ---- a receive loop's dispatch site (rax = channel there) ----
enum : uint8_t { kPeerAtRdi = 0, kPeerInEdi = 1, kPeerServer = 2 };
struct RxSite {
    int32_t data_off;    // rbp + this: the read result's data pointer (server -0x68, client -0x18)
    int32_t size_off;    // rbp + this: its u64 size (server -0x60, client -0x10)
    uintptr_t free_site; // where the loop frees the message and reads the next (the game's own default case)
    uint8_t player;      // 1: the player socket's loop
    uint8_t peer_kind;   // kPeer*: server machine loop [rdi] (u32 machine handle), player loop edi, client: the server
};
// The message at the site; false when there is none to hand on. Reads game memory: in the game, inside a guard.
inline bool rx_read(const KilnRegisters* r, const RxSite& s, const uint8_t** data, uint64_t* size) {
    const uint8_t* d = *reinterpret_cast<const uint8_t* const*>(r->rbp + s.data_off);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(r->rbp + s.size_off);
    if (!d || !n || n > kMaxMessage) return false;
    *data = d;
    *size = n;
    return true;
}
inline uint32_t rx_peer(const KilnRegisters* r, const RxSite& s) {
    if (s.peer_kind == kPeerAtRdi) return *reinterpret_cast<const uint32_t*>(r->rdi);
    if (s.peer_kind == kPeerInEdi) return static_cast<uint32_t>(r->rdi);
    return 0;
}
inline void rx_set_size(KilnRegisters* r, const RxSite& s, uint64_t n) { *reinterpret_cast<uint64_t*>(r->rbp + s.size_off) = n; }
inline void rx_drop(KilnRegisters* r, const RxSite& s) { r->rip = s.free_site; }

// ---- the handler chain ----
constexpr uint8_t kSideServer = 1, kSideClient = 2;
struct HookEntry {
    std::atomic<uint64_t> handle{0};      // 0: free
    KilnOwner owner = 0;
    KilnReplMessageFn fn = nullptr;
    void* user = nullptr;
    uint8_t channel = 0, directions = 0, side = 0;  // side: kSide* of the thread that registered it
    std::atomic<bool> removed{false}, faulted{false}, logged{false};
    std::atomic<uintptr_t> fault_where{0};
    std::atomic<uint32_t> bad{0};         // answers that were neither pass, drop nor a rewrite that fits
};
struct ChainResult {
    int32_t action;   // KILN_REPL_PASS, _DROP, or _REWRITE (data holds the new bytes)
    uint32_t length;  // the message's length now
    uint32_t calls;
};

namespace detail {
inline int note(EXCEPTION_POINTERS* e, uintptr_t* where) {
    *where = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
// fn(m, user) under its own guard: 0 when it returned (its answer in *answer), else the faulting address.
inline uintptr_t call(KilnReplMessageFn fn, const KilnReplMessage* m, void* user, int32_t* answer) {
    uintptr_t where = 0;
    __try {
        *answer = fn(m, user);
    } __except (note(GetExceptionInformation(), &where)) {
        if (!where) where = 1;
    }
    return where;
}
}  // namespace detail

// Runs the live hooks of (side, direction, channel) in order on data (the game's own buffer). Each sees the bytes as the
// hooks before it left them and gets a copy in scratch to rewrite (capacity: the length, at most scratch_cap). A drop
// ends the chain. A rewrite that fits replaces the bytes in place. A fault keeps the bytes as they were (its edits stayed
// in the scratch), flags the hook (it is not called again) and the chain goes on: a fault is a pass. Any other answer
// counts as bad and is a pass.
inline ChainResult run_chain(HookEntry* hooks, uint32_t n, uint8_t side, uint8_t dir, uint8_t channel, uint32_t peer,
                             uint8_t player_socket, uint8_t* data, uint32_t length, uint8_t* scratch, uint32_t scratch_cap) {
    ChainResult res{KILN_REPL_PASS, length, 0};
    bool rewritten = false;
    for (uint32_t i = 0; i < n; ++i) {
        HookEntry& h = hooks[i];
        if (!h.handle.load(std::memory_order_acquire) || h.removed.load(std::memory_order_acquire) ||
            h.faulted.load(std::memory_order_acquire))
            continue;
        if (h.channel != channel || !(h.directions & dir) || !(h.side & side) || !h.fn) continue;
        const uint32_t cap = res.length < scratch_cap ? res.length : scratch_cap;
        std::memcpy(scratch, data, cap);
        uint32_t new_len = res.length;
        KilnReplMessage m{};
        m.size = sizeof m;
        m.peer = peer;
        m.channel = channel;
        m.direction = dir;
        m.player_socket = player_socket;
        m.length = res.length;
        m.data = data;
        m.rewrite = scratch;
        m.rewrite_cap = cap;
        m.rewrite_length = &new_len;
        int32_t answer = KILN_REPL_PASS;
        ++res.calls;
        if (const uintptr_t where = detail::call(h.fn, &m, h.user, &answer)) {
            uintptr_t none = 0;
            h.fault_where.compare_exchange_strong(none, where, std::memory_order_relaxed);
            h.faulted.store(true, std::memory_order_release);
            continue;
        }
        if (answer == KILN_REPL_PASS) continue;
        if (answer == KILN_REPL_DROP) {
            res.action = KILN_REPL_DROP;
            return res;
        }
        if (answer == KILN_REPL_REWRITE && new_len <= cap) {
            std::memcpy(data, scratch, new_len);
            res.length = new_len;
            rewritten = true;
            continue;
        }
        h.bad.fetch_add(1, std::memory_order_relaxed);
    }
    if (rewritten) res.action = KILN_REPL_REWRITE;
    return res;
}


// ---- since package 15's C2 (schema.cpp, messages.cpp): small plain helpers ----
// A game channel's slot in a machine's socket array: the array is indexed by the session's composed channel, which is
// the game channel plus a fixed offset; kiln.net knows it for Kiln's channel 12 (net/channel). -1: outside the array.
inline int socket_index(uint32_t game_channel, int64_t kiln_composed, uint64_t count) {
    if (kiln_composed < 12 || game_channel >= 25) return -1;
    const int64_t i = kiln_composed - 12 + game_channel;
    return i >= 0 && static_cast<uint64_t>(i) < count && count <= 64 ? static_cast<int>(i) : -1;
}
// A bit cursor's message size in bytes: whole 32-bit words.
inline uint32_t cursor_bytes(uint64_t bits) { return static_cast<uint32_t>((bits + 31) / 32 * 4); }
// Whether 0x28 bytes look like a message schema record {name; u32 size; u32 flags 0; fields; u64 count; u64 0}: the
// pointers inside [lo, hi), a size and count in range. The name is checked separately (readable, a C identifier).
inline bool schema_record_shape(const uint8_t* rec, uintptr_t lo, uintptr_t hi) {
    const auto name = at<uintptr_t>(rec, kSchemaName), fields = at<uintptr_t>(rec, kSchemaFields);
    const auto size = at<uint32_t>(rec, kSchemaSize), flags = at<uint32_t>(rec, kSchemaFlags);
    const auto count = at<uint64_t>(rec, kSchemaCount), tail = at<uint64_t>(rec, 0x20);
    return name >= lo && name < hi && fields >= lo && fields < hi && size >= 1 && size <= kMaxSchemaSize && flags == 0 &&
           count >= 1 && count <= kMaxFields && tail == 0;
}
inline bool schema_name_ok(const char* s, size_t max) {
    size_t n = 0;
    for (; n < max && s[n]; ++n) {
        const char c = s[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':')) return false;
    }
    return n >= 3 && n < max && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'));
}

}  // namespace repl::msg
