// SPDX-License-Identifier: MIT
#include "../presentation.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
using namespace kiln::tools;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
static Family family(uint32_t tool = 100) {
    return {tool, "A terrain tool", "Its own description", {{11, 8, 16}, {22, 66, 5}}, {{101, 0, 1.0}, {102, 0, 2.0}}};
}
int main() {
    Registry registry;
    CHECK(registry.add(1, family()));
    CHECK(!registry.add(2, family()));
    auto collision = family(200); collision.brushes[0].item = 101;
    CHECK(!registry.add(2, collision));
    auto other = family(200); other.brushes[0].item = 201; other.brushes[1].item = 202;
    other.materials = {{33, 69, 7}};
    CHECK(registry.add(2, other));
    CHECK(registry.root(101) == 100);
    CHECK(registry.root(100) == 100);
    CHECK(registry.root(201) == 200);
    CHECK(registry.root(0xbe8b0750) == 0); // Vanilla family is not claimed.
    CHECK(registry.family(100)->name == "A terrain tool");
    CHECK(registry.family(100)->description == "Its own description");
    CHECK(registry.brushes(100) == std::vector<uint32_t>({101, 102}));
    CHECK(registry.brushes(0xbe8b0750).empty());

    Selection selection{7, 1, 100, 11};
    std::vector<Stock> stock{{11, 3, 4}, {22, 0, 9}, {999, 999, 999}};
    auto first = registry.snapshot(1, 100, 7, 10, selection, stock);
    CHECK(first.count == 2);
    CHECK(first.rows[0].backpack == 3 && first.rows[0].magic_chests == 4);
    CHECK(first.rows[1].backpack == 0 && first.rows[1].magic_chests == 9);
    CHECK(first.selected_item == 11 && first.selected_material == 8);
    stock[0] = {11, 9999, 9999}; stock.clear(); stock.shrink_to_fit();
    CHECK(first.rows[0].backpack == 3 && first.rows[0].magic_chests == 4); // No borrowed preview view.
    auto stale = registry.snapshot(1, 100, 8, 11, selection, {});
    CHECK(stale.selected_item == 0 && stale.selected_material == 0);
    auto empty = registry.snapshot(1, 100, 7, 12, selection, {});
    CHECK(empty.count == 2 && empty.selected_item == 0);
    CHECK(empty.rows[0].backpack == 0 && empty.rows[0].magic_chests == 0);
    CHECK(!registry.select(empty, 11, selection));
    auto switched = registry.snapshot(1, 200, 7, 13, selection, {{33, 2, 0}});
    CHECK(switched.selected_item == 0); // Never inherit another tool's selection.
    CHECK(!registry.select(switched, 11, selection));
    CHECK(registry.select(switched, 33, selection));
    CHECK(selection.tool == 200 && selection.item == 33 && selection.actor == 1 && selection.session == 7);
    CHECK(!registry.resolve(selection, 1, 100, 7));
    CHECK(!registry.resolve(selection, 2, 200, 7));
    CHECK(!registry.resolve(selection, 1, 200, 8));
    CHECK(registry.resolve(selection, 1, 200, 7)->material == 69);
    auto unlisted = registry.snapshot(1, 100, 7, 14, Selection{7, 1, 100, 999}, {{999, 100, 100}});
    CHECK(unlisted.selected_item == 0 && unlisted.rows[0].backpack == 0);
    auto sums = registry.snapshot(1, 100, 7, 15, {}, {{11, UINT64_MAX, 0}, {11, 1, 1}});
    CHECK(sums.rows[0].backpack == UINT64_MAX && sums.rows[0].magic_chests == 1);
    registry.remove_owner(1);
    CHECK(registry.root(101) == 0 && registry.family(100) == nullptr);
    CHECK(registry.root(201) == 200);

    auto bad = family(); bad.materials.resize(65, {11, 8, 16});
    CHECK(!registry.add(1, bad));
    bad = family(); bad.materials.push_back({11, 9, 16}); CHECK(!registry.add(1, bad));
    bad = family(); bad.materials[0].material = 128; CHECK(!registry.add(1, bad));
    bad = family(); bad.materials[0].voxels_per_item = 0; CHECK(!registry.add(1, bad));
    bad = family(); bad.brushes[0].radius = std::numeric_limits<double>::quiet_NaN(); CHECK(!registry.add(1, bad));
    bad = family(); bad.brushes[0].radius = 0; CHECK(!registry.add(1, bad));
    CHECK(!registry.add(0, family()));
    SelectorGroup group{};
    group.data = group.inline_rows; group.capacity = 64; group.count = 3;
    group.inline_rows[0].item = 999; group.inline_rows[0].count = 100;
    group.inline_rows[1].item = 11; group.inline_rows[1].count = 7;
    group.inline_rows[2].item = 22; group.inline_rows[2].count = 9;
    group.selected = 0; group.has_selected = 1;
    CHECK(filter_selector(group, family().materials));
    CHECK(group.count == 2 && group.data[0].item == 11 && group.data[0].count == 7);
    CHECK(!group.has_selected && group.selected == 0);
    group.selected = 1; group.has_selected = 1;
    CHECK(filter_selector(group, family().materials));
    CHECK(group.has_selected && group.selected == 1);
    CHECK(filter_selector(group, other.materials));
    CHECK(group.count == 0 && !group.has_selected);
    group.count = 65; CHECK(!filter_selector(group, family().materials));
    CHECK(group.count == 0 && !group.has_selected);

    PreviewRow expired_stack[2]{}; expired_stack[0].item = 999; expired_stack[1].item = 11;
    PreviewOwner preview{}; preview.owned.data = preview.owned.inline_rows; preview.owned.count = 2; preview.owned.capacity = 64;
    preview.owned.inline_rows[0] = expired_stack[0]; preview.owned.inline_rows[1] = expired_stack[1];
    preview.owned.inline_rows[1].count = 7;
    preview.view = expired_stack; preview.view_count = 2; preview.selected_item = 999;
    CHECK(filter_preview(preview, family().materials));
    CHECK(preview.view == preview.owned.inline_rows && preview.view_count == 1);
    CHECK(preview.view[0].item == 11 && preview.view[0].count == 7);
    CHECK(preview.selected_item == 0 && preview.selected_count == 0);
    expired_stack[1].item = 999;
    CHECK(preview.view[0].item == 11);
    preview.owned.count = 65; CHECK(!filter_preview(preview, family().materials));
    CHECK(preview.view_count == 0 && preview.owned.count == 0);
    std::cout << "presentation: " << checks << " checks passed\n";
}
