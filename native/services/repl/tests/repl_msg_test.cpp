// SPDX-License-Identifier: MIT
// kiln.repl.net stream C host tests (no game; clang-cl, run under plain Wine by tests/run.sh, so SEH is real): the
// message records' layout; which channels take hooks and inject (and on which socket); the static check of the game's
// message schemas (type codes 0..16 known, 2 and 17+ refused, shapes, nested schemas, enums, bit fields, bounds, depth);
// the count check before an encode; the receive dispatch site's register edits (drop = rip to the free site, rewrite =
// the size written back, the peer from [rdi] / edi / the one server); the handler chain (pass, drop, rewrite in place,
// a fault = pass and the hook flagged, a bad answer = pass, order, other channels/directions/sides untouched).
#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "../msgcore.h"
#include "../repl.h"
#include "services/kiln_repl.h"

namespace repl {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
std::atomic<uintptr_t> g_gs{0};
uint32_t side() { return KILN_SIDE_SERVER; }
uint32_t peer_of(uint32_t, uint8_t*) { return 0; }
void logf(int, const char*, ...) {}
void describe(uintptr_t, char*, size_t) {}
bool schema_check(const char*, const uint8_t*, uint32_t) { return false; }
}  // namespace repl

namespace {

int g_checks = 0, g_failed = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", what);
    }
}
#define CHECK(x) check((x), #x)

using namespace repl::msg;

// ---- fake schemas in the game's layout ----
struct FakeField {
    const char* name;
    uint32_t type, element, shape;
    uint64_t offset, count_off, fixed, bits;
    const void* nested;
};
static_assert(sizeof(FakeField) == 0x40 && offsetof(FakeField, type) == 8 && offsetof(FakeField, element) == 0xc &&
              offsetof(FakeField, shape) == 0x10 && offsetof(FakeField, offset) == 0x18 && offsetof(FakeField, count_off) == 0x20 &&
              offsetof(FakeField, fixed) == 0x28 && offsetof(FakeField, bits) == 0x30 && offsetof(FakeField, nested) == 0x38);
struct FakeSchema {
    const char* name;
    uint32_t size, flags;
    const FakeField* fields;
    uint64_t count, zero;
};
static_assert(sizeof(FakeSchema) == 0x28);
struct FakeEnum {
    const char* name;
    uint64_t size;
};
const uint8_t* S(const FakeSchema& s) { return reinterpret_cast<const uint8_t*>(&s); }

const FakeField kChatFields[] = {
    {"type", U8, EU8, Scalar, 0, 0, 1, 0, nullptr},
    {"string", U8, EU8, Dynamic, 8, 4, 1024, 0, nullptr},
    {"locaTag1", U32, EU32, Scalar, 0x408, 0, 1, 0, nullptr},
    {"locaTag2", U32, EU32, Scalar, 0x40c, 0, 1, 0, nullptr},
};
const FakeSchema kChat{"ChatMessageClientMessage", 0x410, 0, kChatFields, 4, 0};

const FakeField kGuidFields[] = {
    {"data1", U32, EU32, Scalar, 0, 0, 1, 0, nullptr},
    {"data2", U16, EU16, Scalar, 4, 0, 1, 0, nullptr},
    {"data3", U16, EU16, Scalar, 6, 0, 1, 0, nullptr},
    {"data4", U8, EU8, Scalar, 8, 0, 8, 0, nullptr},
};
const FakeSchema kGuid{"Guid", 0x10, 0, kGuidFields, 4, 0};
const FakeEnum kLoginType{"GameMessageLoginType", 1};
const FakeField kLoginFields[] = {
    {"type", Enum, EEnum, Scalar, 0, 0, 1, 3, &kLoginType},
    {"username", U8, EU8, Dynamic, 8, 4, 256, 0, nullptr},
    {"characterId", Struct, EStruct, Optional, 0x10c, 0x108, 1, 0, &kGuid},
    {"inputToggleConfig", U64, EU64, Optional, 0x128, 0x120, 1, 0, nullptr},
    {"buildVersion", U32, EU32, Optional, 0x134, 0x130, 1, 0, nullptr},
    {"isNewCharacter", Bool, EBool, Optional, 0x139, 0x138, 1, 0, nullptr},
};
const FakeSchema kLogin{"GameMessageLoginRequest", 0x140, 0, kLoginFields, 6, 0};

// every wire type Kiln knows, once
const FakeField kAllFields[] = {
    {"b", Bool, EBool, Scalar, 0, 0, 1, 0, nullptr},
    {"f", Float, EFloat, Scalar, 4, 0, 1, 0, nullptr},
    {"s8", S8, ES8, Scalar, 8, 0, 1, 0, nullptr},
    {"u8", U8, EU8, Scalar, 9, 0, 1, 0, nullptr},
    {"s16", S16, ES16, Scalar, 10, 0, 1, 0, nullptr},
    {"u16", U16, EU16, Scalar, 12, 0, 1, 0, nullptr},
    {"s32", S32, ES32, Scalar, 16, 0, 1, 0, nullptr},
    {"u32", U32, EU32, Scalar, 20, 0, 1, 0, nullptr},
    {"s64", S64, ES64, Scalar, 24, 0, 1, 0, nullptr},
    {"u64", U64, EU64, Scalar, 32, 0, 1, 0, nullptr},
    {"str", String, EChar, Scalar, 40, 0, 16, 0, nullptr},
    {"guid", Struct, EStruct, Scalar, 56, 0, 1, 0, &kGuid},
    {"kind", Enum, EEnum, Scalar, 72, 0, 1, 3, &kLoginType},
    {"rawguid", Guid, EGuid, Scalar, 80, 0, 1, 0, nullptr},
    {"bits", Bits, EU16, Scalar, 96, 0, 1, 12, nullptr},
    {"var", VarU32, EU32, Scalar, 100, 0, 1, 0, nullptr},
};
const FakeSchema kAll{"AllTypes", 104, 0, kAllFields, 16, 0};

FakeField g_bad[1];
FakeSchema g_badSchema{"Bad", 0x40, 0, g_bad, 1, 0};
bool one_field_ok(FakeField f, uint32_t size = 0x40) {
    g_bad[0] = f;
    g_badSchema.size = size;
    char why[128] = {};
    return validate(S(g_badSchema), why, sizeof why);
}

void abi() {
    static_assert(sizeof(KilnReplMessage) == 48 && offsetof(KilnReplMessage, length) == 12 &&
                  offsetof(KilnReplMessage, rewrite) == 24 && offsetof(KilnReplMessage, rewrite_cap) == 32);
    static_assert(sizeof(KilnReplSchemaField) == 48 && offsetof(KilnReplSchemaField, type) == 16 &&
                  offsetof(KilnReplSchemaField, nested) == 40);
    CHECK(KILN_REPL_NET_CAP_MESSAGES_SEND == 32 && KILN_REPL_NET_CAP_MESSAGES_RECEIVE == 64 && KILN_REPL_NET_CAP_SCHEMA == 128);
}

void channels() {
    const uint32_t refused[] = {4, 5, 12, 14, 17, 25, 200};
    for (uint32_t c : refused) CHECK(!hookable(c));
    uint32_t n = 0;
    for (uint32_t c = 0; c < 25; ++c) n += hookable(c);
    CHECK(n == 20);
    const uint32_t inj[] = {1, 15, 19, 20, 21};
    uint32_t m = 0;
    for (uint32_t c = 0; c < 32; ++c) m += injectable(c);
    CHECK(m == 5);
    for (uint32_t c : inj) CHECK(injectable(c));
    CHECK(!injectable(0) && !injectable(4) && !injectable(12) && !injectable(17));
    CHECK(inject_socket(1) == kMachineSocket && inject_socket(15) == kMachineSocket && inject_socket(19) == kMachineSocket &&
          inject_socket(20) == kMachineSocket && inject_socket(21) == kPlayerSocket);
    CHECK(inject_socket(9) == kPlayerSocket && inject_socket(0) == kMachineSocket && inject_socket(14) == 0 && inject_socket(17) == 0);
}

void schemas() {
    char why[128] = {};
    CHECK(validate(S(kChat), why, sizeof why));
    CHECK(validate(S(kLogin), why, sizeof why));
    CHECK(validate(S(kAll), why, sizeof why));
    CHECK(element_size(EEnum, reinterpret_cast<const uint8_t*>(&kLoginType)) == 1);
    CHECK(element_size(EStruct, S(kGuid)) == 0x10 && element_size(EGuid, nullptr) == 16 && element_size(EChar, nullptr) == 1);
    CHECK(element_size(99, nullptr) == 0);
    // unknown wire types: 2 (the game's own coder stops the process on it) and 17+
    CHECK(!one_field_ok({"d", Double, EDouble, Scalar, 0, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", 17, EU32, Scalar, 0, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", 0x40, EU32, Scalar, 0, 0, 1, 0, nullptr}));
    // shapes: 3+ refused; an optional with more than one element; a dynamic string
    CHECK(!one_field_ok({"x", U32, EU32, 3, 0, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", U32, EU32, Optional, 4, 0, 2, 0, nullptr}));
    CHECK(one_field_ok({"x", U32, EU32, Optional, 4, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", String, EChar, Dynamic, 8, 4, 16, 0, nullptr}));
    // the element's storage must fit the wire coding
    CHECK(!one_field_ok({"x", U32, EU8, Scalar, 0, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", U64, EU32, Scalar, 0, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", String, EU32, Scalar, 0, 0, 16, 0, nullptr}));
    CHECK(!one_field_ok({"x", Guid, EU64, Scalar, 0, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", VarU32, EU16, Scalar, 0, 0, 1, 0, nullptr}));
    // nested: a struct without its schema, an enum without its descriptor or of an odd size
    CHECK(!one_field_ok({"x", Struct, EStruct, Scalar, 0, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", Enum, EEnum, Scalar, 0, 0, 1, 3, nullptr}));
    static const FakeEnum odd{"Odd", 3};
    CHECK(!one_field_ok({"x", Enum, EEnum, Scalar, 0, 0, 1, 3, &odd}));
    CHECK(!one_field_ok({"x", Enum, EEnum, Scalar, 0, 0, 1, 33, &kLoginType}));  // more bits than the encoder's u32
    // bit fields: unsigned storage only, no more bits than it holds
    CHECK(one_field_ok({"x", Bits, EU16, Scalar, 0, 0, 1, 16, nullptr}));
    CHECK(!one_field_ok({"x", Bits, EU16, Scalar, 0, 0, 1, 17, nullptr}));
    CHECK(!one_field_ok({"x", Bits, ES16, Scalar, 0, 0, 1, 8, nullptr}));
    // bounds: storage past the struct, a count or present flag past it, a dynamic capacity over the wire's 16 bits
    CHECK(!one_field_ok({"x", U32, EU32, Scalar, 0x3e, 0, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", U8, EU8, Dynamic, 8, 4, 0x39, 0, nullptr}));
    CHECK(one_field_ok({"x", U8, EU8, Dynamic, 8, 4, 0x38, 0, nullptr}));
    CHECK(!one_field_ok({"x", U8, EU8, Dynamic, 8, 0x3e, 4, 0, nullptr}));
    CHECK(!one_field_ok({"x", U8, EU8, Dynamic, 8, 4, 0x10000, 0, nullptr}, 0x10010));
    CHECK(!one_field_ok({"x", U32, EU32, Optional, 0, 0x40, 1, 0, nullptr}));
    CHECK(!one_field_ok({"x", String, EChar, Scalar, 0x30, 0, 0x11, 0, nullptr}));
    // a nested schema whose flags are not 0 (the game reads its size as a qword) and a schema that nests itself
    static FakeField inner[1] = {{"a", U32, EU32, Scalar, 0, 0, 1, 0, nullptr}};
    static FakeSchema flagged{"Flagged", 4, 1, inner, 1, 0};
    CHECK(!one_field_ok({"x", Struct, EStruct, Scalar, 0, 0, 1, 0, &flagged}));
    static FakeField self_f[1];
    static FakeSchema self{"Self", 0x40, 0, self_f, 1, 0};
    self_f[0] = {"me", Struct, EStruct, Optional, 8, 0, 1, 0, &self};
    CHECK(!validate(S(self), why, sizeof why));
    // no fields, too many, a huge size
    static FakeSchema empty{"Empty", 4, 0, inner, 0, 0};
    CHECK(!validate(S(empty), why, sizeof why));
    static FakeSchema huge{"Huge", 0x200000, 0, inner, 1, 0};
    CHECK(!validate(S(huge), why, sizeof why));
    // the reason says which field
    g_bad[0] = {"thefield", Double, EDouble, Scalar, 0, 0, 1, 0, nullptr};
    g_badSchema.size = 0x40;
    why[0] = 0;
    validate(S(g_badSchema), why, sizeof why);
    CHECK(std::strstr(why, "thefield") != nullptr);
}

void counts() {
    std::vector<uint8_t> m(0x410, 0);
    uint32_t c = 1024;
    std::memcpy(m.data() + 4, &c, 4);
    CHECK(counts_ok(S(kChat), m.data()));
    c = 1025;
    std::memcpy(m.data() + 4, &c, 4);
    CHECK(!counts_ok(S(kChat), m.data()));
    // arrays of structs with arrays inside: every element's count is checked
    static const FakeField innerF[] = {{"n", U8, EU8, Dynamic, 4, 0, 8, 0, nullptr}};
    static const FakeSchema innerS{"Inner", 12, 0, innerF, 1, 0};
    static const FakeField outerF[] = {{"items", Struct, EStruct, Dynamic, 4, 0, 3, 0, &innerS}};
    static const FakeSchema outerS{"Outer", 40, 0, outerF, 1, 0};
    char why[128] = {};
    CHECK(validate(S(outerS), why, sizeof why));
    std::vector<uint8_t> o(40, 0);
    uint32_t n = 3;
    std::memcpy(o.data(), &n, 4);
    uint32_t k = 8;
    std::memcpy(o.data() + 4 + 12 * 2, &k, 4);  // the third element's count
    CHECK(counts_ok(S(outerS), o.data()));
    k = 9;
    std::memcpy(o.data() + 4 + 12 * 2, &k, 4);
    CHECK(!counts_ok(S(outerS), o.data()));
    n = 2;  // the third element is not in use: its count does not matter
    std::memcpy(o.data(), &n, 4);
    CHECK(counts_ok(S(outerS), o.data()));
}

// ---- the receive dispatch site ----
void rx_site() {
    alignas(16) uint8_t frame[0x100] = {};
    uint8_t msg[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uintptr_t rbp = reinterpret_cast<uintptr_t>(frame) + 0x90;
    const RxSite server{-0x68, -0x60, 0x7be0e4, 0, kPeerAtRdi};
    const uint8_t* p = msg;
    uint64_t n = 12;
    std::memcpy(reinterpret_cast<void*>(rbp - 0x68), &p, 8);
    std::memcpy(reinterpret_cast<void*>(rbp - 0x60), &n, 8);
    KilnRegisters r{};
    r.rbp = rbp;
    r.rip = 0x1234;
    uint32_t machine = 0x81;
    r.rdi = reinterpret_cast<uintptr_t>(&machine);
    const uint8_t* d = nullptr;
    uint64_t len = 0;
    CHECK(rx_read(&r, server, &d, &len) && d == msg && len == 12);
    CHECK(rx_peer_key(&r, server) == 0x81);
    rx_set_size(&r, server, 8);
    uint64_t back = 0;
    std::memcpy(&back, reinterpret_cast<void*>(rbp - 0x60), 8);
    CHECK(back == 8 && r.rip == 0x1234);
    rx_drop(&r, server);
    CHECK(r.rip == 0x7be0e4);
    const RxSite player{-0x68, -0x60, 0x7be74d, 1, kPeerInEdi};
    r.rdi = 0xdead00000042ull;
    CHECK(rx_peer_key(&r, player) == 0x42);
    const RxSite client{-0x18, -0x10, 0x875641, 0, kPeerServer};
    std::memcpy(reinterpret_cast<void*>(rbp - 0x18), &p, 8);
    std::memcpy(reinterpret_cast<void*>(rbp - 0x10), &n, 8);
    CHECK(rx_read(&r, client, &d, &len) && d == msg && len == 12);
    CHECK(rx_peer_key(&r, client) == 0);
    // no data, or a size past the largest message: not a message to hand to anyone
    const uint8_t* none = nullptr;
    std::memcpy(reinterpret_cast<void*>(rbp - 0x18), &none, 8);
    CHECK(!rx_read(&r, client, &d, &len));
    std::memcpy(reinterpret_cast<void*>(rbp - 0x18), &p, 8);
    uint64_t big = kMaxMessage + 1;
    std::memcpy(reinterpret_cast<void*>(rbp - 0x10), &big, 8);
    CHECK(!rx_read(&r, client, &d, &len));
}

// Exercise the installed send and receive hooks against a stand-in GameServer and session.
struct TestAlloc {
    uint64_t status = 0, handle = 0, h1 = 0, h2 = 0;
    uint32_t size = 4, pad = 0;
    uint8_t* data;
    uint64_t capacity = 4;
};
using TestAllocFn = TestAlloc* (*)(TestAlloc*, uintptr_t, uintptr_t, const void*);
using TestCommitFn = void (*)(uintptr_t, uint64_t*);
TestAlloc* alloc_original(TestAlloc* out, uintptr_t, uintptr_t, const void*) { return out; }
void commit_original(uintptr_t, uint64_t*) {}

void player_socket_peer() {
    static TestAllocFn alloc = nullptr;
    static TestCommitFn commit = nullptr;
    static KilnMidFn receive = nullptr;
    static void* receive_site = nullptr;
    static KilnHooksApi hooks{};
    hooks.header = {sizeof hooks, KILN_HOOKS_VERSION};
    hooks.raw_detour = [](KilnOwner, uintptr_t target, const KilnHookOrder*, void* fn, void** original, KilnHandle* out) -> KilnResult {
        if (target == reinterpret_cast<uintptr_t>(&alloc_original)) {
            alloc = reinterpret_cast<TestAllocFn>(fn);
            *original = reinterpret_cast<void*>(&alloc_original);
        } else {
            commit = reinterpret_cast<TestCommitFn>(fn);
            *original = reinterpret_cast<void*>(&commit_original);
        }
        *out = target;
        return KILN_OK;
    };
    hooks.mid2 = [](KilnOwner, uintptr_t target, const KilnHookOrder*, KilnMidFn fn, void* site, KilnHandle* out) -> KilnResult {
        if (target == 2) {
            receive = fn;
            receive_site = site;
        }
        *out = target;
        return KILN_OK;
    };
    static KilnApi api{};
    api.size = sizeof api;
    api.on_tick_thread = [] { return true; };
    api.service = [](KilnStr, uint32_t) -> const void* { return &hooks; };
    api.feature = [](KilnStr) { return true; };
    api.symbol = [](KilnStr name, uintptr_t* out) -> KilnResult {
        const std::string_view n(name.ptr, name.len);
        if (n == "net/SessionAlloc") *out = reinterpret_cast<uintptr_t>(&alloc_original);
        else if (n == "net/SessionCommit" || n == "net/SessionAbort") *out = reinterpret_cast<uintptr_t>(&commit_original);
        else *out = n == "repl/ServerPlayerDispatch" ? 2 : 1;
        return KILN_OK;
    };
    api.constant = [](KilnStr name, int64_t* out) -> KilnResult {
        const std::string_view n(name.ptr, name.len);
        const struct { std::string_view name; int64_t value; } constants[] = {
            {"net/channel", 12}, {"net/gs.sockets", 0x30}, {"net/gs.records", 0x1e0},
            {"net/rec.stride", 0x148}, {"net/rec.count", 16}, {"net/rec.peer", 4},
            {"net/after_sync.gs_register", 13},
            {"net/sockets.machines", 0x28}, {"net/machine.stride", 0x20}, {"net/machine.sockets", 0x10},
        };
        for (const auto& c : constants) {
            if (c.name != n) continue;
            *out = c.value;
            return KILN_OK;
        }
        return KILN_ERR_NOT_FOUND;
    };
    repl::g_api = &api;
    repl::messages_start();
    KilnHandle handle = 0;
    KilnReplMessage observed{};
    CHECK(repl::t_message_hook(0, 21, KILN_REPL_SEND | KILN_REPL_RECEIVE,
        [](const KilnReplMessage* m, void* out) -> int32_t {
            *static_cast<KilnReplMessage*>(out) = *m;
            return KILN_REPL_PASS;
        }, &observed, &handle) == KILN_OK);
    CHECK(alloc && commit && receive);
    if (!alloc || !commit || !receive) return;

    constexpr uint32_t slot = 2, player = 0x42, peer = 0x183;
    struct Socket { uint8_t kind; uint8_t pad[3]; uint32_t channel, index; } socket{kPlayerSocket, {}, 21, slot};
    uintptr_t sockets[25]{};
    sockets[21] = reinterpret_cast<uintptr_t>(&socket);
    struct SocketEntry { uint32_t handle; uint32_t pad[3]; uintptr_t* sockets; uint64_t count; } players[64]{}, machines[128]{};
    players[slot] = {player, {}, sockets, 25};
    machines[peer & 0x7f].handle = peer;
    uintptr_t session = 0;
    uintptr_t set[6]{reinterpret_cast<uintptr_t>(&session), 0, reinterpret_cast<uintptr_t>(players), 0, 0,
                     reinterpret_cast<uintptr_t>(machines)};
    alignas(8) uint8_t gs[0x1e0 + 16 * 0x148]{};
    const auto set_address = reinterpret_cast<uintptr_t>(set);
    std::memcpy(gs + 0x30, &set_address, sizeof set_address);
    const uint32_t record[]{player, peer};
    std::memcpy(gs + 0x1e0 + slot * 0x148, record, sizeof record);
    repl::g_gs = reinterpret_cast<uintptr_t>(gs);

    uint8_t data[4]{};
    auto send = [&](uintptr_t to) {
        TestAlloc out{.data = data};
        observed = {};
        alloc(&out, to, reinterpret_cast<uintptr_t>(&socket), nullptr);
        commit(to, &out.handle);
        CHECK(observed.direction == KILN_REPL_SEND && observed.player_socket == 1);
    };
    send(set[0]);
    CHECK(observed.peer == peer && observed.peer_flags == 0);
    send(set[0] + sizeof session);  // the GameServer's record belongs to a different session
    CHECK(observed.peer == 0 && observed.peer_flags == KILN_REPL_MSG_PEER_UNKNOWN);
    sockets[21] = 0;  // the allocation's socket no longer belongs to the slot
    send(set[0]);
    CHECK(observed.peer == 0 && observed.peer_flags == KILN_REPL_MSG_PEER_UNKNOWN);
    sockets[21] = reinterpret_cast<uintptr_t>(&socket);

    alignas(8) uint8_t frame[0x80]{};
    const uint8_t* payload = data;
    const uint64_t length = sizeof data;
    std::memcpy(frame + 0x18, &payload, sizeof payload);
    std::memcpy(frame + 0x20, &length, sizeof length);
    KilnRegisters r{};
    r.rax = 21;
    r.rbp = reinterpret_cast<uintptr_t>(frame) + sizeof frame;
    r.r13 = reinterpret_cast<uintptr_t>(gs);
    auto recv = [&](uint32_t from) {
        r.rdi = 0xdead00000000ull | from;
        observed = {};
        receive(&r, receive_site);
        CHECK(observed.direction == KILN_REPL_RECEIVE && observed.player_socket == 1);
    };
    recv(player);
    CHECK(observed.peer == peer && observed.peer_flags == 0);
    recv(player + 0x40);  // same slot, different generation
    CHECK(observed.peer == 0 && observed.peer_flags == KILN_REPL_MSG_PEER_UNKNOWN);
    machines[peer & 0x7f].handle += 0x80;  // the machine slot now belongs to a different peer
    recv(player);
    CHECK(observed.peer == 0 && observed.peer_flags == KILN_REPL_MSG_PEER_UNKNOWN);
    repl::g_gs = 0;
}

// ---- the handler chain ----
int g_calls[8];
int32_t KILN_CALL h_pass(const KilnReplMessage* m, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    return m->size == sizeof(KilnReplMessage) ? KILN_REPL_PASS : 99;
}
int32_t KILN_CALL h_drop(const KilnReplMessage*, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    return KILN_REPL_DROP;
}
// upper-cases the bytes and cuts the last one
int32_t KILN_CALL h_upper_cut(const KilnReplMessage* m, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    for (uint32_t i = 0; i < m->length && i < m->rewrite_cap; ++i)
        m->rewrite[i] = (m->data[i] >= 'a' && m->data[i] <= 'z') ? static_cast<uint8_t>(m->data[i] - 32) : m->data[i];
    *m->rewrite_length = m->length - 1;
    return KILN_REPL_REWRITE;
}
uint8_t g_seen[64];
uint32_t g_seen_len = 0;
int32_t KILN_CALL h_record(const KilnReplMessage* m, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    g_seen_len = m->length;
    std::memcpy(g_seen, m->data, m->length < sizeof g_seen ? m->length : sizeof g_seen);
    return KILN_REPL_PASS;
}
int32_t KILN_CALL h_too_long(const KilnReplMessage* m, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    m->rewrite[0] = 'Z';
    *m->rewrite_length = m->rewrite_cap + 1;
    return KILN_REPL_REWRITE;
}
#ifdef _WIN32
int32_t KILN_CALL h_fault(const KilnReplMessage* m, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    m->rewrite[0] = 'Q';  // edits the rewrite buffer, then faults: the edit must not reach the message
    volatile int* bad = nullptr;
    *bad = 1;
    return KILN_REPL_REWRITE;
}
#endif
int32_t KILN_CALL h_odd(const KilnReplMessage*, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    return 7;
}

void add(HookEntry* hooks, uint32_t i, KilnReplMessageFn fn, uintptr_t tag, uint8_t channel, uint8_t dirs, uint8_t side) {
    hooks[i].fn = fn;
    hooks[i].user = reinterpret_cast<void*>(tag);
    hooks[i].channel = channel;
    hooks[i].directions = dirs;
    hooks[i].side = side;
    hooks[i].removed.store(false);
    hooks[i].faulted.store(false);
    hooks[i].handle.store(0x4d48000000000000ull | (i + 1));
}

void chain() {
    uint8_t scratch[64];
    {   // pass, then a rewrite in place, then a second handler sees the rewritten bytes; other keys untouched
        HookEntry hooks[8];
        std::memset(g_calls, 0, sizeof g_calls);
        add(hooks, 0, h_pass, 0, 19, KILN_REPL_SEND, kSideServer);
        add(hooks, 1, h_upper_cut, 1, 19, KILN_REPL_SEND | KILN_REPL_RECEIVE, kSideServer);
        add(hooks, 2, h_record, 2, 19, KILN_REPL_SEND, kSideServer);
        add(hooks, 3, h_drop, 3, 15, KILN_REPL_SEND, kSideServer);       // another channel
        add(hooks, 4, h_drop, 4, 19, KILN_REPL_RECEIVE, kSideServer);    // another direction
        add(hooks, 5, h_drop, 5, 19, KILN_REPL_SEND, kSideClient);       // another side
        add(hooks, 6, h_drop, 6, 19, KILN_REPL_SEND, kSideServer);
        hooks[6].removed.store(true);                                    // removed: never called
        uint8_t data[12] = {'k', 'i', 'l', 'n', 't', 'e', 's', 't', '-', 'u', 'p', 0};
        const ChainResult res = run_chain(hooks, 8, kSideServer, KILN_REPL_SEND, 19, 0x81, 0, data, 12, scratch, sizeof scratch);
        CHECK(res.action == KILN_REPL_REWRITE && res.length == 11);
        CHECK(std::memcmp(data, "KILNTEST-UP", 11) == 0);
        CHECK(g_seen_len == 11 && std::memcmp(g_seen, "KILNTEST-UP", 11) == 0);
        CHECK(g_calls[0] == 1 && g_calls[1] == 1 && g_calls[2] == 1 && g_calls[3] == 0 && g_calls[4] == 0 && g_calls[5] == 0 &&
              g_calls[6] == 0);
    }
    {   // a drop stops the chain
        HookEntry hooks[3];
        std::memset(g_calls, 0, sizeof g_calls);
        add(hooks, 0, h_drop, 0, 19, KILN_REPL_RECEIVE, kSideClient);
        add(hooks, 1, h_record, 1, 19, KILN_REPL_RECEIVE, kSideClient);
        uint8_t data[4] = {1, 2, 3, 4};
        const ChainResult res = run_chain(hooks, 3, kSideClient, KILN_REPL_RECEIVE, 19, 0, 0, data, 4, scratch, sizeof scratch);
        CHECK(res.action == KILN_REPL_DROP && g_calls[0] == 1 && g_calls[1] == 0);
    }
#ifdef _WIN32
    {   // a fault: pass, the bytes are the game's, the hook is flagged (and never called again); the next one runs
        HookEntry hooks[3];
        std::memset(g_calls, 0, sizeof g_calls);
        add(hooks, 0, h_fault, 0, 20, KILN_REPL_SEND, kSideServer);
        add(hooks, 1, h_record, 1, 20, KILN_REPL_SEND, kSideServer);
        uint8_t data[4] = {'a', 'b', 'c', 'd'};
        ChainResult res = run_chain(hooks, 3, kSideServer, KILN_REPL_SEND, 20, 0x81, 0, data, 4, scratch, sizeof scratch);
        CHECK(res.action == KILN_REPL_PASS && std::memcmp(data, "abcd", 4) == 0);
        CHECK(hooks[0].faulted.load() && hooks[0].fault_where.load() != 0 && g_calls[1] == 1);
        CHECK(g_seen_len == 4 && std::memcmp(g_seen, "abcd", 4) == 0);
        res = run_chain(hooks, 3, kSideServer, KILN_REPL_SEND, 20, 0x81, 0, data, 4, scratch, sizeof scratch);
        CHECK(g_calls[0] == 1 && g_calls[1] == 2);
    }
#endif
    {   // bad answers: a rewrite longer than allowed, an unknown action, a size field the handler must see right
        HookEntry hooks[3];
        std::memset(g_calls, 0, sizeof g_calls);
        add(hooks, 0, h_too_long, 0, 21, KILN_REPL_SEND, kSideServer);
        add(hooks, 1, h_odd, 1, 21, KILN_REPL_SEND, kSideServer);
        add(hooks, 2, h_pass, 2, 21, KILN_REPL_SEND, kSideServer);
        uint8_t data[4] = {'a', 'b', 'c', 'd'};
        const ChainResult res = run_chain(hooks, 3, kSideServer, KILN_REPL_SEND, 21, 0x81, 0, data, 4, scratch, sizeof scratch);
        CHECK(res.action == KILN_REPL_PASS && std::memcmp(data, "abcd", 4) == 0);
        CHECK(hooks[0].bad.load() == 1 && hooks[1].bad.load() == 1 && hooks[2].bad.load() == 0 && g_calls[2] == 1);
    }
    {   // a message bigger than the scratch: handlers see it, but cannot rewrite more than the scratch holds
        HookEntry hooks[2];
        std::memset(g_calls, 0, sizeof g_calls);
        add(hooks, 0, h_upper_cut, 0, 1, KILN_REPL_SEND, kSideServer);
        std::vector<uint8_t> big(200, 'x');
        const ChainResult res = run_chain(hooks, 2, kSideServer, KILN_REPL_SEND, 1, 0x81, 0, big.data(), 200, scratch, sizeof scratch);
        CHECK(res.action == KILN_REPL_PASS && big[0] == 'x' && g_calls[0] == 1 && hooks[0].bad.load() == 1);
    }
    {   // the record a handler gets
        HookEntry hooks[1];
        add(hooks, 0, h_pass, 0, 15, KILN_REPL_RECEIVE, kSideServer);
        std::memset(g_calls, 0, sizeof g_calls);
        uint8_t data[8] = {};
        const ChainResult res = run_chain(hooks, 1, kSideServer, KILN_REPL_RECEIVE, 15, 0x42, 1, data, 8, scratch, sizeof scratch);
        CHECK(res.action == KILN_REPL_PASS && hooks[0].bad.load() == 0 && g_calls[0] == 1);
    }
}

}  // namespace

void helpers() {
    // socket_index: Kiln's channel 12 at composed 22 -> game channel 19 at 29; outside the array -> -1
    CHECK(socket_index(19, 22, 40) == 29);
    CHECK(socket_index(1, 22, 40) == 11);
    CHECK(socket_index(19, 22, 25) == -1);
    CHECK(socket_index(25, 22, 64) == -1);
    CHECK(socket_index(1, 0, 64) == -1);
    // cursor_bytes: whole words
    CHECK(cursor_bytes(0) == 0 && cursor_bytes(1) == 4 && cursor_bytes(32) == 4 && cursor_bytes(33) == 8);
    // schema_record_shape
    alignas(8) uint8_t rec[0x28] = {};
    auto put64 = [&](size_t o, uint64_t v) { std::memcpy(rec + o, &v, 8); };
    auto put32 = [&](size_t o, uint32_t v) { std::memcpy(rec + o, &v, 4); };
    put64(0, 0x140100000); put32(8, 0x108); put32(12, 0); put64(16, 0x140200000); put64(24, 3); put64(32, 0);
    CHECK(schema_record_shape(rec, 0x140000000, 0x150000000));
    put32(12, 1);
    CHECK(!schema_record_shape(rec, 0x140000000, 0x150000000));  // flags must be 0
    put32(12, 0); put64(32, 7);
    CHECK(!schema_record_shape(rec, 0x140000000, 0x150000000));  // the tail qword is 0
    put64(32, 0); put64(24, 0);
    CHECK(!schema_record_shape(rec, 0x140000000, 0x150000000));  // no fields
    put64(24, 3); put64(0, 0x100);
    CHECK(!schema_record_shape(rec, 0x140000000, 0x150000000));  // name outside the image
    CHECK(schema_name_ok("ChatMessageServerMessage", 128) && schema_name_ok("keen::Foo", 128));
    CHECK(!schema_name_ok("ab", 128) && !schema_name_ok("9Lives", 128) && !schema_name_ok("bad name", 128));
}

namespace review {
HookEntry hooks[2];
ScratchStack stack;
uint8_t out[16];
int32_t KILN_CALL send_pass(const KilnReplMessage*, void*) { return KILN_REPL_PASS; }
int32_t KILN_CALL recv_rewrite_then_send(const KilnReplMessage* m, void*) {
    std::memcpy(m->rewrite, "HELLO", 5);
    *m->rewrite_length = 5;
    std::memcpy(out, "XXXXXXXXXX", 10);
    uint8_t* s = stack.push();  // the nested send chain gets a scratch of its own
    if (s) run_chain(hooks, 2, kSideServer, KILN_REPL_SEND, 19, 1, 0, out, 10, s, kMaxMessage);
    stack.pop();
    return KILN_REPL_REWRITE;
}
int32_t KILN_CALL shorten(const KilnReplMessage* m, void*) {
    std::memcpy(m->rewrite, "abc", 3);
    *m->rewrite_length = 3;
    return KILN_REPL_REWRITE;
}
}  // namespace review

void review_fixes() {
    using namespace review;
    // should 5: a nested send chain does not clobber the receive chain's rewrite
    hooks[0].handle = 1; hooks[0].fn = &recv_rewrite_then_send; hooks[0].channel = 19; hooks[0].directions = KILN_REPL_RECEIVE; hooks[0].side = kSideServer;
    hooks[1].handle = 2; hooks[1].fn = &send_pass; hooks[1].channel = 19; hooks[1].directions = KILN_REPL_SEND; hooks[1].side = kSideServer;
    uint8_t data[8];
    std::memcpy(data, "abcdefgh", 8);
    uint8_t* s = stack.push();
    CHECK(s != nullptr);
    const ChainResult r = s ? run_chain(hooks, 2, kSideServer, KILN_REPL_RECEIVE, 19, 1, 0, data, 8, s, kMaxMessage) : ChainResult{};
    stack.pop();
    CHECK(r.action == KILN_REPL_REWRITE && r.length == 5 && std::memcmp(data, "HELLO", 5) == 0);
    CHECK(stack.depth == 0);
    // the stack refuses a fourth level
    CHECK(stack.push() && stack.push() && stack.push() && !stack.push());
    stack.pop(); stack.pop(); stack.pop();
    CHECK(stack.depth == 0);
    // should 10: a shorter rewrite (8 -> 3 bytes)
    HookEntry one[1];
    one[0].handle = 3; one[0].fn = &shorten; one[0].channel = 19; one[0].directions = KILN_REPL_SEND; one[0].side = kSideServer;
    uint8_t msg8[8];
    std::memcpy(msg8, "12345678", 8);
    uint8_t sc[16];
    const ChainResult sr = run_chain(one, 1, kSideServer, KILN_REPL_SEND, 19, 1, 0, msg8, 8, sc, sizeof sc);
    CHECK(sr.action == KILN_REPL_REWRITE && sr.length == 3 && std::memcmp(msg8, "abc", 3) == 0);
    // should 6: a fake session: alloc (ok / refused), commit, abort
    OpenAllocs oa;
    int a1 = 0, d1 = 0, a2 = 0, d2 = 0;
    oa.record(&a1, &d1, 0, 0x100);   // ok: recorded
    oa.record(&a2, &d2, 3, 0x200);   // refused (status 3): not recorded
    CHECK(oa.take(&a2, &d2) == 0);   // a refused allocation is never matched
    CHECK(oa.take(&a1, &d1) == 0x100);
    CHECK(oa.take(&a1, &d1) == 0);   // taken once
    // the same result address with another buffer (a later message at the same stack slot) is not tagged by a stale entry
    oa.record(&a1, &d1, 0, 0x300);
    CHECK(oa.take(&a1, &d2) == 0);
    // full: the oldest goes, the newest is still matched (aborted allocations never commit and must not stop recording)
    int ar[12], dr[12];
    for (int i = 0; i < 12; ++i) oa.record(&ar[i], &dr[i], 0, 0x1000 + i);
    CHECK(oa.take(&ar[11], &dr[11]) == 0x100b);
    CHECK(oa.take(&ar[0], &dr[0]) == 0);
    // should 7: the schema an outgoing message must decode as
    CHECK(send_schema(19, kSideServer) && std::strcmp(send_schema(19, kSideServer), "ChatMessageServerMessage") == 0);
    CHECK(send_schema(19, kSideClient) && std::strcmp(send_schema(19, kSideClient), "ChatMessageClientMessage") == 0);
    CHECK(send_schema(1, kSideServer) && std::strcmp(send_schema(1, kSideServer), "ServerInfoMessage") == 0);
    CHECK(send_schema(9, kSideServer) == nullptr);   // no schema: a rewrite there is refused
    CHECK(send_schema(1, kSideClient) == nullptr);   // a client never sends ServerInfo
}

const FakeField kNameFields[] = {
    {"id", U32, EU32, Scalar, 0, 0, 1, 0, nullptr},
    {"name", String, EChar, Scalar, 4, 0, 8, 0, nullptr},
};
const FakeSchema kNamed{"Named", 12, 0, kNameFields, 2, 0};
const FakeField kListFields[] = {
    {"count", U8, EU8, Scalar, 0, 0, 1, 0, nullptr},
    {"items", Struct, EStruct, Dynamic, 8, 4, 2, 0, &kNamed},
};
const FakeSchema kList{"NamedList", 32, 0, kListFields, 2, 0};

void strings() {
    alignas(8) uint8_t m[12] = {};
    std::memcpy(m + 4, "abc", 3);
    CHECK(strings_terminated(S(kNamed), m));
    std::memcpy(m + 4, "abcdefgh", 8);           // 8 chars in a char[8]: the game would write a NUL at m[12]
    CHECK(!strings_terminated(S(kNamed), m));
    // inside a list of structs: only the elements in use count
    alignas(8) uint8_t l[32] = {};
    uint32_t n = 1;
    std::memcpy(l + 4, &n, 4);
    std::memcpy(l + 8 + 4, "ok", 2);
    std::memcpy(l + 20 + 4, "12345678", 8);      // element 2: not in use (count 1)
    CHECK(strings_terminated(S(kList), l));
    n = 2;
    std::memcpy(l + 4, &n, 4);
    CHECK(!strings_terminated(S(kList), l));
}

void frees() {
    CHECK(may_free(kSideServer, kSideServer));
    CHECK(!may_free(kSideClient, kSideServer));  // a hosted world: the client world's hook, the server's Pre
    CHECK(!may_free(kSideServer, kSideClient));
    CHECK(may_free(kSideServer | kSideClient, kSideServer | kSideClient));
    CHECK(!may_free(kSideServer | kSideClient, kSideServer));
    CHECK(!may_free(kSideServer, 0));
}

int main() {
    strings();
    frees();
    review_fixes();
    helpers();
    abi();
    channels();
    schemas();
    counts();
    rx_site();
    chain();
    player_socket_peer();
    std::printf("repl-msg-test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
