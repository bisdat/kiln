// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_items_storage.h"
#include <atomic>
#include <cstdint>
namespace kitems {
// Published once at boot. Separate from the engine adapter so existing fake-engine tests need no storage service.
struct StorageGuards {
    bool (*contains)(uint32_t) = nullptr;
    bool (*holder)(uint32_t) = nullptr;
    bool (*accepts)(uint32_t, uint32_t, bool, uint64_t) = nullptr;
    bool (*actor)(uint32_t, uint32_t) = nullptr;
    uint32_t (*root)(uint32_t) = nullptr;
    bool (*available)(uint32_t) = nullptr;
};
inline std::atomic<const StorageGuards*> storage_guards{nullptr};
inline bool storage_contains(uint32_t entity) {
    auto g = storage_guards.load();
    return g && g->contains && g->contains(entity);
}
inline bool storage_holder(uint32_t entity) {
    auto g = storage_guards.load();
    return g && g->holder && g->holder(entity);
}
inline bool storage_accepts(uint32_t target, uint32_t item, bool pide, uint64_t slot = 0) {
    auto g = storage_guards.load();
    return !g || !g->accepts || g->accepts(target, item, pide, slot);
}
inline bool storage_mutable(uint32_t entity) {
    auto g = storage_guards.load();
    return !g || !g->available || g->available(entity);
}
inline uint32_t storage_root(uint32_t entity) {
    auto g = storage_guards.load();
    return g && g->root ? g->root(entity) : 0;
}
inline bool storage_actor(uint32_t target, uint32_t actor) {
    auto g = storage_guards.load();
    return !g || !g->actor || g->actor(target, actor);
}
} // namespace kitems
