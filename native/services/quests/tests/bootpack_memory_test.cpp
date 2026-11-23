// SPDX-License-Identifier: MIT
// The real compiler and publisher, with synthetic resources only: no growing history of knowledge maps.
#include "../bootpack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <tuple>

namespace {
using namespace kiln::qb;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (0)
void put32(Bytes& b, size_t at, uint32_t value) { std::memcpy(b.data() + at, &value, 4); }
void array(Bytes& b, size_t field, size_t bytes, uint32_t count) {
    const size_t at = b.size();
    b.resize(at + bytes);
    put32(b, field, static_cast<uint32_t>(at - field));
    put32(b, field + 4, count);
}
Bytes base() {
    Bytes b(72);
    for (const auto& [field, stride, id] : {std::tuple<size_t, size_t, uint32_t>{0, 12, 1}, {24, 24, 2}}) {
        array(b, field, 8, 1); put32(b, b.size() - 4, 1);
        array(b, field + 8, 4, 1); put32(b, b.size() - 4, id);
        array(b, field + 16, stride, 1); put32(b, b.size() - stride, id);
    }
    // Unknown, unchanged data must stay at the original offset, not be trimmed with old Kiln tables.
    array(b, 48, 4, 1); put32(b, b.size() - 4, 0xabcdef12);
    array(b, 56, 4, 1); put32(b, b.size() - 4, 0x12345678);
    b[64] = 0x5a;
    return b;
}
template<class T> size_t retained_bytes(const T& before) {
    if constexpr (requires { before.bytes; }) return before.bytes.capacity();
    else return 0;
}
struct Store {
    struct Layer { uint64_t owner; Guid guid; uint32_t type; Bytes bytes; };
    Bytes original = base();
    std::vector<Layer> layers;
    uint64_t next_owner = 0;
    bool corrupt_restore = false, foreign_write = false;
    bootpack::Backend backend() {
        bootpack::Backend b;
        b.game.read = [this](const Guid& guid, uint32_t type, const uint8_t** p, size_t* n) {
            for (auto it = layers.rbegin(); it != layers.rend(); ++it)
                if (it->guid == guid && it->type == type) { *p = it->bytes.data(); *n = it->bytes.size(); return true; }
            Guid knowledge; parse_guid(kGuidKnowledge, &knowledge);
            if (guid != knowledge || type != kTypeKnowledge) return false;
            *p = original.data(); *n = original.size(); return true;
        };
        b.game.template_by_name = [](const std::string&, Guid*) { return false; };
        b.owner = [this](const std::string&) { return ++next_owner; };
        b.begin = [](uint64_t, const std::string&, std::string*) { return true; };
        b.put = [this](uint64_t owner, const bootpack::Resource& resource, std::string*) {
            layers.push_back({owner, resource.guid, resource.type, *resource.bytes}); return true;
        };
        b.define = [](uint64_t, bool, const std::string&, std::string*) { return true; };
        b.text = [](uint64_t, const TextOut&, std::string*) { return true; };
        b.finish = [this](uint64_t owner, std::string*) {
            if (foreign_write && owner == 1) {
                auto foreign = layers.back();
                foreign.owner = 1000;
                array(foreign.bytes, 48, 4, 1);
                put32(foreign.bytes, foreign.bytes.size() - 4, 0xdecafbad);
                layers.push_back(std::move(foreign));
            }
            return true;
        };
        b.rollback = [this](uint64_t owner, std::string*) {
            std::erase_if(layers, [&](const Layer& layer) { return layer.owner == owner; });
            if (corrupt_restore && layers.empty()) original.back() ^= 1;
            return true;
        };
        return b;
    }
};
void compact_layers() {
    Store store;
    const auto backend = store.backend();
    std::vector<ModInput> mods;
    for (unsigned i = 0; i < 10; ++i) {
        ModInput mod;
        mod.id = "memory-" + std::to_string(i);
        mod.documents = {ojson{{"knowledge", {{"seen", {{"scope", "both"}}}}}}};
        mods.push_back(std::move(mod));
    }
    const auto once = compile_pack(mods, backend.game);
    CHECK(once.ok && once.any);
    const auto result = bootpack::build(mods, backend);
    CHECK(!result.stopped && result.errors.empty());
    CHECK(result.applied.size() == mods.size());
    size_t before_bytes = 0, layer_bytes = 0;
    for (const auto& applied : result.applied)
        for (const auto& before : applied.before) before_bytes += retained_bytes(before);
    CHECK(before_bytes == 0);
    for (const auto& layer : store.layers) {
        layer_bytes += layer.bytes.size();
        CHECK(layer.bytes.size() <= once.res_knowledge.size());
        CHECK(std::equal(store.original.begin() + 48, store.original.end(), layer.bytes.begin() + 48));
    }
    CHECK(layer_bytes <= mods.size() * once.res_knowledge.size());
    if (!store.layers.empty()) {
        const auto& bytes = store.layers.back().bytes;
        for (const auto& def : once.knowledge) {
            CHECK(knowledge_find(bytes.data(), bytes.size(), false, def.id) != nullptr);
            CHECK(knowledge_find(bytes.data(), bytes.size(), true, def.id) != nullptr);
        }
    }
    for (auto it = result.applied.rbegin(); it != result.applied.rend(); ++it) {
        std::string why;
        CHECK(bootpack::withdraw(*it, backend, &why));
    }
    CHECK(store.layers.empty() && store.original == base());
}
void foreign_appended_data_starts_a_new_prefix() {
    Store store;
    store.foreign_write = true;
    const auto backend = store.backend();
    std::vector<ModInput> mods;
    for (unsigned i = 0; i < 3; ++i) {
        ModInput mod;
        mod.id = "foreign-" + std::to_string(i);
        mod.documents = {ojson{{"knowledge", {{"seen", {{"scope", "both"}}}}}}};
        mods.push_back(std::move(mod));
    }
    const auto result = bootpack::build(mods, backend);
    CHECK(!result.stopped && result.errors.empty());
    CHECK(result.applied.size() == 3 && store.layers.size() == 4);
    const auto check_foreign = [&] {
        const auto& bytes = store.layers.back().bytes;
        uint32_t offset = 0, value = 0;
        std::memcpy(&offset, bytes.data() + 48, 4);
        const size_t at = 48 + offset;
        CHECK(at + 4 <= bytes.size());
        if (at + 4 <= bytes.size()) {
            std::memcpy(&value, bytes.data() + at, 4);
            CHECK(value == 0xdecafbad);
        }
    };
    if (!store.layers.empty()) check_foreign();
    if (!result.applied.empty()) {
        std::string why;
        CHECK(bootpack::withdraw(result.applied.back(), backend, &why));
        check_foreign();
    }
}
void bad_compaction_input_changes_nothing() {
    Bytes bytes = base();
    const auto original = bytes;
    std::string why;
    CHECK(!compact_knowledge(&bytes, 64, &why));
    CHECK(bytes == original);
    put32(bytes, 4, 3); // a bucket count that is not a power of two
    const auto malformed = bytes;
    CHECK(!compact_knowledge(&bytes, original.size(), &why));
    CHECK(bytes == malformed);
}
void fingerprint_checks_bytes_not_only_size() {
    Store store;
    const auto backend = store.backend();
    ModInput mod;
    mod.id = "fingerprint";
    mod.documents = {ojson{{"knowledge", {{"seen", ojson::object()}}}}};
    const auto result = bootpack::build({mod}, backend);
    CHECK(result.applied.size() == 1);
    if (result.applied.empty()) return;
    store.corrupt_restore = true;
    std::string why;
    CHECK(!bootpack::withdraw(result.applied.front(), backend, &why));
    CHECK(why.find("before-image") != std::string::npos);
}
}
int main() {
    compact_layers();
    foreign_appended_data_starts_a_new_prefix();
    bad_compaction_input_changes_nothing();
    fingerprint_checks_bytes_not_only_size();
    std::printf("bootpack memory: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
