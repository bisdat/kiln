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
#include <vector>

#include "../msgcore.h"
#include "services/kiln_repl.h"

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
    CHECK(rx_peer(&r, server) == 0x81);
    rx_set_size(&r, server, 8);
    uint64_t back = 0;
    std::memcpy(&back, reinterpret_cast<void*>(rbp - 0x60), 8);
    CHECK(back == 8 && r.rip == 0x1234);
    rx_drop(&r, server);
    CHECK(r.rip == 0x7be0e4);
    const RxSite player{-0x68, -0x60, 0x7be74d, 1, kPeerInEdi};
    r.rdi = 0xdead00000042ull;
    CHECK(rx_peer(&r, player) == 0x42);
    const RxSite client{-0x18, -0x10, 0x875641, 0, kPeerServer};
    std::memcpy(reinterpret_cast<void*>(rbp - 0x18), &p, 8);
    std::memcpy(reinterpret_cast<void*>(rbp - 0x10), &n, 8);
    CHECK(rx_read(&r, client, &d, &len) && d == msg && len == 12);
    CHECK(rx_peer(&r, client) == 0);
    // no data, or a size past the largest message: not a message to hand to anyone
    const uint8_t* none = nullptr;
    std::memcpy(reinterpret_cast<void*>(rbp - 0x18), &none, 8);
    CHECK(!rx_read(&r, client, &d, &len));
    std::memcpy(reinterpret_cast<void*>(rbp - 0x18), &p, 8);
    uint64_t big = kMaxMessage + 1;
    std::memcpy(reinterpret_cast<void*>(rbp - 0x10), &big, 8);
    CHECK(!rx_read(&r, client, &d, &len));
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
int32_t KILN_CALL h_fault(const KilnReplMessage* m, void* u) {
    ++g_calls[reinterpret_cast<uintptr_t>(u)];
    m->rewrite[0] = 'Q';  // edits the rewrite buffer, then faults: the edit must not reach the message
    volatile int* bad = nullptr;
    *bad = 1;
    return KILN_REPL_REWRITE;
}
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

int main() {
    abi();
    channels();
    schemas();
    counts();
    rx_site();
    chain();
    std::printf("repl-msg-test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
