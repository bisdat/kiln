// SPDX-License-Identifier: MIT
#include "../boot_text_undo.h"
#include <cstdio>
#include <map>

namespace {
int checks = 0, failed = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #value); } } while (0)
}
int main() {
    using Undo = kc::bootpack::TextUndo<const int*>;
    using Map = std::map<uint32_t, const int*>;
    int original = 1, first = 2, last = 3, foreign = 4;
    Undo undo;
    CHECK(undo.record(42, &original, &first));
    CHECK(undo.record(42, &first, &last));
    CHECK(undo.record(43, nullptr, &first));
    Map current{{42, &last}, {43, &first}, {99, &foreign}}, restored;
    std::string why;
    CHECK(undo.restore(current, &restored, [](uint32_t) { return true; }, &why));
    CHECK(restored.at(42) == &original);
    CHECK(!restored.count(43));
    CHECK(restored.at(99) == &foreign);
    CHECK(current.at(42) == &last); // planning the restore never changes the served map

    current[42] = &foreign;
    restored = {{7, &original}};
    const auto sentinel = restored;
    CHECK(!undo.restore(current, &restored, [](uint32_t) { return true; }, &why));
    CHECK(why.find("42") != std::string::npos);
    CHECK(restored == sentinel); // neither one tag nor an output shard was partly restored
    CHECK(current.at(42) == &foreign);
    CHECK(!undo.record(42, &foreign, &first));
    CHECK(undo.changes.at(42).before == &original);
    CHECK(undo.changes.at(42).after == &last);

    current[42] = &last;
    CHECK(undo.restore(current, &restored, [](uint32_t tag) { return tag == 43; }, &why));
    CHECK(restored.at(42) == &last);
    CHECK(!restored.count(43));
    CHECK(restored.at(99) == &foreign);
    undo.recording = false;
    CHECK(!undo.record(50, nullptr, &first));
    CHECK(!undo.changes.count(50));
    std::printf("boot text undo: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
