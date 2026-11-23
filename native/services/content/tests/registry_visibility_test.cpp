// SPDX-License-Identifier: MIT
#include "../registry/visibility.h"
#include <cstdio>
#include <cstring>
using namespace kc::registry;
int main() {
    int passed = 0, failed = 0;
    auto check = [&](bool yes, const char* name) { if (yes) ++passed; else { ++failed; std::printf("FAIL %s\n", name); } };
    alignas(16) unsigned char arena[1024]{}; const auto address = reinterpret_cast<uintptr_t>(arena);
    auto record = [&](size_t at, uint32_t id, size_t text) { std::memcpy(arena + at, &id, 4); const int32_t offset = static_cast<int32_t>(text - at - 4); std::memcpy(arena + at + 4, &offset, 4); };
    record(0, 1, 800); record(8, 2, 808); record(16, 3, 816);
    FilterIo io{[&](uintptr_t at, void* out, size_t n) { if (at < address || n > sizeof arena || at - address > sizeof arena - n) return false; std::memcpy(out, reinterpret_cast<void*>(at), n); return true; },
                [&](uintptr_t, size_t n) -> void* { return n <= 512 ? arena + 256 : nullptr; }};
    std::string why;
    auto filtered = filter_array(address, 2, 8, 0, {address + 16}, {4}, {2}, io, &why);
    check(filtered && filtered->count == 2, "loot/recipe filtering excludes hidden records while retaining extras");
    if (filtered && filtered->count == 2) {
        uint32_t first = 0, second = 0; int32_t offset = 0;
        std::memcpy(&first, reinterpret_cast<void*>(filtered->data), 4); std::memcpy(&second, reinterpret_cast<void*>(filtered->data + 8), 4);
        std::memcpy(&offset, reinterpret_cast<void*>(filtered->data + 4), 4);
        check(first == 1 && second == 3, "filtering preserves original order and visible extra records");
        check(filtered->data + 4 + static_cast<intptr_t>(offset) == address + 800, "self-relative strings keep the original backing target");
    }
    check(arena[8] == 2, "original game records are not deleted or modified");
    filtered = filter_array(address, 2, 8, 0, {}, {4}, {1, 2}, io, &why);
    check(filtered && !filtered->data && !filtered->count, "hiding every candidate gives a valid empty view");
    filtered = filter_array(address, 2, 8, 7, {}, {4}, {}, io, &why);
    check(!filtered, "invalid identity layout fails closed");
    auto no_room = io; no_room.allocate = [](auto, auto) -> void* { return nullptr; };
    check(!filter_array(address, 2, 8, 0, {}, {4}, {1}, no_room, &why), "allocation failure never returns the original unfiltered list");
    auto unreadable = io; unreadable.read = [](auto, auto, auto) { return false; };
    check(!filter_array(address, 2, 8, 0, {}, {4}, {1}, unreadable, &why), "unreadable lists fail closed");
    check(!filter_array(address, SIZE_MAX, 8, 0, {}, {4}, {}, io, &why), "input work and multiplication are bounded");
    auto visible = visible_recipes({11, 12, 13}, {2}, [](uintptr_t p) { return std::optional<uint32_t>(static_cast<uint32_t>(p - 10)); });
    check(visible == std::vector<uint64_t>({11, 13}), "workshop compaction keeps visible recipe pointers in order");
    std::printf("registry-visibility-test: %d passed, %d failed\n", passed, failed); return failed ? 1 : 0;
}
