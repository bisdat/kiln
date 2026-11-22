// SPDX-License-Identifier: MIT
// The part of a named-service snapshot or compare-exchange that runs while kiln.world holds the engine's write gate
// (named_services.h). No wait, allocation, lock, log or KilnApi call in this file, and every loop has a fixed bound: the
// profile's record, map-capacity and system bounds (at most 1024, 4096 and 2048, checked by the generator), the 64 access
// views of one system, or the service's size (at most the gate's write budget, checked first). tests/lint_hold.py checks
// the file.
//
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer): modules/game-runtime/native/named-services/
// resolve_named_service.cpp (named, resolve_binding, resolve: the same reads in the same order, the same refusals) and
// read_and_update_named_service.cpp:26-85 (the read, the compare, the merge, the write, the readback, the rollback).
#include <cstring>

#include "named_services.h"

namespace kw::named {

#ifdef KILN_WORLD_HOLD_TEST_POINTS
void (*g_hold_test_point)(HoldPoint) = nullptr;
#define KILN_NAMED_POINT(p) \
    do {                     \
        if (g_hold_test_point) g_hold_test_point(p); \
    } while (0)
#else
#define KILN_NAMED_POINT(p) \
    do {                     \
    } while (0)
#endif

namespace {
using kiln::engine::ServiceContract;
using kiln::engine::ServiceLayout;

// An uncertain outcome latches the service for the view's session before the gate is released (Services::latched), so
// no writer that checked the latch before this release writes after it (the check before the write below).
void uncertain(const HoldInput& in, HoldOutput& out, std::uint32_t why) {
    out.status = KILN_ECS_UNCERTAIN;
    out.why = why;
    if (in.latch && in.session) in.latch->store(in.session, std::memory_order_release);
}

bool add(std::uintptr_t base, std::size_t offset, std::uintptr_t& address) {
    if (!base || offset > UINTPTR_MAX - base) return false;
    address = base + offset;
    return true;
}
template <class T> bool field(const HoldInput& in, std::uintptr_t base, std::size_t offset, T& value) {
    std::uintptr_t at = 0;
    if (!add(base, offset, at) || at > UINTPTR_MAX - sizeof value) return false;
    return in.copy(at, &value, sizeof value) == KILN_OK;
}
// The engine's name (pointer, length) is the contract's key, byte for byte.
bool named(const HoldInput& in, std::uintptr_t string, std::uint64_t length, const ServiceContract& c) {
    char bytes[kiln::engine::kServiceKeyBytes];
    return length == c.key_size && length < sizeof bytes && in.copy(string, bytes, length) == KILN_OK &&
           std::memcmp(bytes, c.key.data(), length) == 0;
}

// resolve_binding: the service's record through the registry owner's name map, its one binding, and no second record
// of the same name.
bool resolve_binding(const HoldInput& in, std::uintptr_t registry, std::uintptr_t& payload) {
    const ServiceLayout& L = *in.layout;
    const ServiceContract& c = *in.contract;
    std::uintptr_t rows{}, bits{}, keys{}, values{};
    std::uint64_t count{}, capacity{};
    std::uint32_t used{};
    if (!field(in, registry, L.records, rows) || !rows || !field(in, registry, L.record_count, count) || !count ||
        count > L.maximum_records || !field(in, registry, L.map_bits, bits) || !bits ||
        !field(in, registry, L.map_capacity, capacity) || !capacity || capacity > L.maximum_capacity ||
        (capacity & (capacity - 1)) || !field(in, registry, L.map_keys, keys) || !keys ||
        !field(in, registry, L.map_values, values) || !values || !field(in, registry, L.map_count, used) || !used ||
        used > count || used > capacity || rows > UINTPTR_MAX - count * L.stride)
        return false;
    std::size_t slot = static_cast<std::size_t>(c.key_hash) & (capacity - 1);
    std::uintptr_t record{};
    for (std::size_t visited = 0; visited < capacity; ++visited) {
        std::uint64_t word{};
        std::uint32_t hash{};
        if (!field(in, bits, (slot / 64) * 8, word) || !(word & (std::uint64_t{1} << (slot % 64))) ||
            !field(in, keys, slot * 4, hash))
            return false;
        if (hash == c.key_hash) {
            std::uint16_t index{};
            if (!field(in, values, slot * 2, index) || index >= count || rows > UINTPTR_MAX - index * L.stride)
                return false;
            record = rows + index * L.stride;
            break;
        }
        slot = (slot + 1) & (capacity - 1);
    }
    std::uintptr_t name{}, alternate{}, resolved{};
    std::uint64_t length{};
    std::uint32_t hash{};
    if (!record || !field(in, record, 0, name) || !field(in, record, 8, length) || !named(in, name, length, c) ||
        !field(in, record, L.record_hash, hash) || hash != c.key_hash || !field(in, record, L.normal, resolved) ||
        !resolved || !field(in, record, L.alternate, alternate) || alternate != resolved)
        return false;
    // Duplicate full names as well as hash collisions in the original records are refused, instead of trusting the
    // original hash-only lookup blindly.
    unsigned matches{};
    for (std::size_t index = 0; index < count; ++index) {
        std::uintptr_t row{};
        if (!add(rows, index * L.stride, row) || !field(in, row, 0, name) || !field(in, row, 8, length)) return false;
        if (named(in, name, length, c) && ++matches > 1) return false;
    }
    if (matches != 1) return false;
    payload = resolved;
    return true;
}
}  // namespace

bool resolve(const HoldInput& in, std::uintptr_t& payload) noexcept {
    const ServiceLayout& L = *in.layout;
    const ServiceContract& c = *in.contract;
    const kiln::engine::EcsView& owner = in.scope->gate().expected;
    const std::uintptr_t image = owner.owners.image;
    const std::uintptr_t registry = owner.registry_owner;
    // The type's metadata through its registry slot: the contract's size and hash.
    std::uintptr_t metadata{};
    std::uint32_t size{}, type_hash{};
    if (!image || !field(in, image, c.metadata_slot, metadata) || metadata != image + c.metadata ||
        !field(in, metadata, 0x40, size) || size != c.size || !field(in, metadata, 0x50, type_hash) ||
        type_hash != c.type_hash || !resolve_binding(in, registry, payload))
        return false;
    // Exactly one system writes the service: the contract's descriptor and function, the service its one mutable view,
    // both binding tables pointing at the same payload.
    std::uintptr_t name{};
    std::uint64_t length{};
    std::uint16_t systems{};
    if (!field(in, registry, L.system_count, systems) || !systems || systems > L.maximum_systems) return false;
    unsigned bound{};
    for (std::size_t index = 0; index < systems; ++index) {
        std::uintptr_t system{};
        if (!add(registry, L.systems + index * L.system_stride, system) || system > UINTPTR_MAX - L.system_stride)
            return false;
        std::uintptr_t descriptor{};
        if (!field(in, system, 0, descriptor)) return false;
        if (descriptor != image + c.descriptor) continue;
        if (++bound != 1) return false;
        std::uintptr_t function{}, reads{}, writes{};
        std::uint64_t read_count{}, write_count{};
        if (!field(in, system, 0x28, function) || function != image + c.function || !field(in, descriptor, 0x98, reads) ||
            !field(in, descriptor, 0xa0, read_count) || !field(in, descriptor, 0xb0, writes) ||
            !field(in, descriptor, 0xb8, write_count) || read_count > 64 || !write_count || read_count + write_count > 64)
            return false;
        std::size_t ordinal{};
        unsigned mutable_matches{};
        for (std::size_t position = 0; position < read_count + write_count; ++position) {
            std::uintptr_t view{};
            if (!add(position < read_count ? reads : writes,
                     (position < read_count ? position : position - read_count) * 16, view) ||
                !field(in, view, 0, name) || !field(in, view, 8, length))
                return false;
            if (!named(in, name, length, c)) continue;
            if (position < read_count || ++mutable_matches != 1) return false;
            ordinal = position;
        }
        std::uint8_t normal_count{}, alternate_count{};
        std::uintptr_t normal_value{}, alternate_value{};
        if (mutable_matches != 1 || !field(in, system, L.normal_count, normal_count) ||
            !field(in, system, L.alternate_count, alternate_count) || normal_count != read_count + write_count ||
            alternate_count != normal_count || !field(in, system, L.normal_bindings + ordinal * 8, normal_value) ||
            !field(in, system, L.alternate_bindings + ordinal * 8, alternate_value) || normal_value != payload ||
            alternate_value != payload)
            return false;
    }
    return bound == 1;
}

void hold(const HoldInput& in, HoldOutput& out) noexcept {
    out = HoldOutput{};
    KILN_NAMED_POINT(HoldPoint::Begin);
    const kiln::engine::EcsGateScope& scope = *in.scope;
    const std::size_t n = in.contract->size;
    // 1. The gate: the GameServer thread, its running Scene and the authority view still hold.
    if (!scope.validate()) {
        out.status = KILN_ECS_UNAVAILABLE;
        out.why = KILN_ECS_WRITE_WHY_GATE;
        return;
    }
    // 1b. A write of a service an uncertain write latched after this call checked the latch: nothing is written.
    if (in.write && in.latch && in.session && in.latch->load(std::memory_order_acquire) == in.session) {
        out.status = KILN_ECS_UNCERTAIN;
        out.why = KILN_ECS_WRITE_WHY_TARGET_LATCHED;
        out.flags = KILN_MUTATION_TARGET_LATCHED;
        return;
    }
    // 2. The gate's budget (ShroudForge: size > MaximumBytes refuses the read and the write).
    if (n > scope.gate().maximum_bytes) {
        out.status = KILN_ECS_NOT_WRITABLE;
        out.why = KILN_ECS_WRITE_WHY_OVER_BUDGET;
        return;
    }
    // 3. The service in this world, in committed private read-write memory.
    std::uintptr_t at = 0, again = 0;
    if (!resolve(in, at)) {
        out.status = KILN_ECS_UNREADABLE;
        out.why = KILN_ECS_WRITE_WHY_REF;
        return;
    }
    if (!in.writable || !in.writable(in.user, at, n)) {
        out.status = KILN_ECS_UNREADABLE;
        out.why = KILN_ECS_WRITE_WHY_FAULT;
        return;
    }
    KILN_NAMED_POINT(HoldPoint::AfterResolve);
    // 4. The current bytes (a fault: nothing written).
    if (in.copy(at, in.before, n) != KILN_OK) {
        out.status = KILN_ECS_UNREADABLE;
        out.why = KILN_ECS_WRITE_WHY_FAULT;
        return;
    }
    // A snapshot: the service found again at the same place, the gate still valid.
    if (!in.write) {
        if (!resolve(in, again) || again != at || !scope.validate()) {
            out.status = KILN_ECS_UNREADABLE;
            out.why = KILN_ECS_WRITE_WHY_REF;
            return;
        }
        out.status = KILN_ECS_OK;
        out.flags = KILN_MUTATION_CONFIRMED;
        return;
    }
    // 5. Compared in full with the caller's expected bytes.
    if (std::memcmp(in.before, in.expected, n) != 0) {
        out.status = KILN_ECS_CONFLICT;
        out.why = KILN_ECS_WRITE_WHY_COMPARE;
        out.flags = KILN_MUTATION_CONFIRMED;
        return;
    }
    KILN_NAMED_POINT(HoldPoint::AfterCompare);
    // 6. The desired bytes merged under the mask; nothing to change is done once the service is proven where it was.
    for (std::size_t i = 0; i < n; ++i) in.merged[i] = (!in.mask || in.mask[i]) ? in.desired[i] : in.before[i];
    if (std::memcmp(in.merged, in.before, n) == 0) {
        if (!resolve(in, again) || again != at || !scope.validate()) {
            out.status = KILN_ECS_UNREADABLE;
            out.why = KILN_ECS_WRITE_WHY_REF;
            return;
        }
        out.status = KILN_ECS_OK;
        out.flags = KILN_MUTATION_NO_OP | KILN_MUTATION_CONFIRMED;
        return;
    }
    // An owner retired since the call began: it may be the caller, so nothing is written.
    if (in.retirements && in.retirements->load(std::memory_order_acquire) != in.retirements_seen) {
        out.status = KILN_ECS_BUSY;
        out.why = KILN_ECS_WRITE_WHY_RETIRE_RACE;
        return;
    }
    // 7. Written under the gate (the budget refuses before any byte). The game's own code writes this plain-data service
    // directly, and its readers read it on every use, without ECS stamps (read_and_update_named_service.cpp:62-63).
    const KilnResult w = scope.write(at, in.merged, n);
    if (w == KILN_ERR_BUFFER_TOO_SMALL) {
        out.status = KILN_ECS_NOT_WRITABLE;
        out.why = KILN_ECS_WRITE_WHY_OVER_BUDGET;
        return;
    }
    if (w == KILN_ERR_INVALID_ARGUMENT) {  // no held gate: nothing was written
        out.status = KILN_ECS_UNAVAILABLE;
        out.why = KILN_ECS_WRITE_WHY_GATE;
        return;
    }
    KILN_NAMED_POINT(HoldPoint::AfterWrite);
    // 8. Proven: the bytes read back, the service found again at the same place, the gate still valid.
    const bool written = w == KILN_OK;
    if (written && in.copy(at, in.verified, n) == KILN_OK && std::memcmp(in.verified, in.merged, n) == 0) {
        if (resolve(in, again) && again == at && scope.validate()) {
            out.status = KILN_ECS_OK;
            out.flags = KILN_MUTATION_WRITTEN | KILN_MUTATION_CONFIRMED;
            return;
        }
        // The owner changed after the write: the address cannot be trusted for a rollback.
        uncertain(in, out, KILN_ECS_WRITE_WHY_READBACK);
        return;
    }
    // 9. Not proven: rolled back in the same hold, and the rollback read back, the service found again.
    scope.write(at, in.before, n);
    KILN_NAMED_POINT(HoldPoint::AfterRollback);
    if (in.copy(at, in.verified, n) == KILN_OK && std::memcmp(in.verified, in.before, n) == 0 && resolve(in, again) &&
        again == at && scope.validate()) {
        out.status = KILN_ECS_UNREADABLE;
        out.why = written ? KILN_ECS_WRITE_WHY_READBACK : KILN_ECS_WRITE_WHY_FAULT;
        out.flags = KILN_MUTATION_ROLLED_BACK;
        return;
    }
    uncertain(in, out, written ? KILN_ECS_WRITE_WHY_READBACK : KILN_ECS_WRITE_WHY_FAULT);
}

}  // namespace kw::named
