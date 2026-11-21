// SPDX-License-Identifier: MIT
// The game's named-service registry inside a fake engine (fake_engine.h), laid out from a profile's typed serviceLayout:
// a decoy record and the contract's record, the registry's name map, a decoy system and the one system bound to the
// service, and the type's metadata in the image. Shared by named_services_test.cpp (host) and named_services_win.cpp
// (Windows).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "fake_engine.h"
#include "include/kiln_engine_layout.h"

namespace namedfake {
using fake::get;
using fake::put;
using kiln::engine::Layout;
using kiln::engine::ServiceContract;

inline uint32_t fnv1a(const std::string& s) {
    uint32_t h = 0x811c9dc5u;
    for (unsigned char c : s) h = (h ^ c) * 0x01000193u;
    return h;
}

struct Registry {
    const Layout& L;
    fake::Engine& e;
    const ServiceContract& C;
    fake::Region rows{4096}, bits{4096}, keys{4096}, values{4096}, names{4096}, payload{4096}, reads{4096}, writes{4096};
    uintptr_t registry;
    Registry(const Layout& layout, fake::Engine& engine)
        : L(layout), e(engine), C(layout.services.contracts[0]), registry(engine.registry_owner()) {
        wire();
    }
    uintptr_t service() const { return payload.at(); }
    uintptr_t system(size_t i) const { return registry + L.services.systems + i * L.services.system_stride; }
    uintptr_t record(size_t i) const { return rows.at(i * L.services.stride); }
    void set_record(size_t i, uintptr_t name, uint64_t length, uint32_t hash, uintptr_t at) {
        const auto& S = L.services;
        put<uint64_t>(record(i), name);
        put<uint64_t>(record(i) + 8, length);
        put<uint32_t>(record(i) + S.record_hash, hash);
        put<uint64_t>(record(i) + S.normal, at);
        put<uint64_t>(record(i) + S.alternate, at);
    }
    void insert(uint32_t hash, uint16_t index) {
        size_t slot = hash & 15;
        for (int i = 0; i < 16 && (get<uint64_t>(bits.at((slot / 64) * 8)) >> (slot % 64) & 1); ++i) slot = (slot + 1) & 15;
        put<uint64_t>(bits.at((slot / 64) * 8), get<uint64_t>(bits.at((slot / 64) * 8)) | (uint64_t{1} << (slot % 64)));
        put<uint32_t>(keys.at(slot * 4), hash);
        put<uint16_t>(values.at(slot * 2), index);
    }
    void wire() {
        const auto& S = L.services;
        std::memset(bits.base, 0, bits.size);
        std::memcpy(names.base, C.key.data(), C.key_size);
        std::memcpy(names.base + 256, "ecs::Other", 10);
        std::memcpy(names.base + 512, "ecs::Clock", 10);
        put<uint64_t>(e.image.at(C.metadata_slot), e.image.at(C.metadata));
        put<uint32_t>(e.image.at(C.metadata) + 0x40, C.size);
        put<uint32_t>(e.image.at(C.metadata) + 0x50, C.type_hash);
        set_record(0, names.at(256), 10, fnv1a("ecs::Other"), payload.at(2048));
        set_record(1, names.at(0), C.key_size, C.key_hash, payload.at());
        put<uint64_t>(registry + S.records, rows.at());
        put<uint64_t>(registry + S.record_count, 2);
        put<uint64_t>(registry + S.map_bits, bits.at());
        put<uint64_t>(registry + S.map_capacity, 16);
        put<uint64_t>(registry + S.map_keys, keys.at());
        put<uint64_t>(registry + S.map_values, values.at());
        put<uint32_t>(registry + S.map_count, 2);
        insert(fnv1a("ecs::Other"), 0);
        insert(C.key_hash, 1);
        put<uint16_t>(registry + S.system_count, 2);
        put<uint64_t>(system(0), e.image.at(C.descriptor + 64));  // another system
        put<uint64_t>(system(1), e.image.at(C.descriptor));
        put<uint64_t>(system(1) + 0x28, e.image.at(C.function));
        const uintptr_t d = e.image.at(C.descriptor);
        put<uint64_t>(d + 0x98, reads.at());
        put<uint64_t>(d + 0xa0, 1);
        put<uint64_t>(d + 0xb0, writes.at());
        put<uint64_t>(d + 0xb8, 1);
        put<uint64_t>(reads.at(0), names.at(512));
        put<uint64_t>(reads.at(8), 10);
        put<uint64_t>(writes.at(0), names.at(0));
        put<uint64_t>(writes.at(8), C.key_size);
        put<uint8_t>(system(1) + S.normal_count, 2);
        put<uint8_t>(system(1) + S.alternate_count, 2);
        put<uint64_t>(system(1) + S.normal_bindings + 8, payload.at());
        put<uint64_t>(system(1) + S.alternate_bindings + 8, payload.at());
    }
};

}  // namespace namedfake
