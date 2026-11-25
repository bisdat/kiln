// SPDX-License-Identifier: MIT
// Real registry transactions -> layered store -> ContentSync's production export, over synthetic game memory.
// The wire/application half is ContentJoinTests.Registry_layers_resync_without_rejoining.
#define main registry_live_suite_main
#include "registry_live_test.cpp"
#undef main

namespace kc {
std::string ref_text(const uint8_t g[16]) { return guid_text(g); }
bool read_recipe_stacks(const Entry& e, uint32_t which, std::vector<KilnContentStack>* out) {
    const uintptr_t header = e.addr + (which == KILN_CONTENT_INPUTS ? RL.input : RL.output);
    int32_t offset = 0; uint32_t count = 0;
    if (!rdv(header, offset) || !rdv(header + 4, count) || count > 256) return false;
    out->clear();
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t p = header + offset + i * (which == KILN_CONTENT_INPUTS ? RL.in_stride : RL.out_stride);
        KilnContentStack s{};
        if (which == KILN_CONTENT_INPUTS) {
            if (!rdv(p + RL.in_item, s.item) || !rdv(p + RL.in_count, s.count) || !rdv(p + RL.in_cat, s.category)) return false;
        } else if (!rdv(p + RL.out_item, s.item) || !rdv(p + RL.out_count, s.count)) return false;
        out->push_back(s);
    }
    return true;
}
} // namespace kc

int main() {
    std::vector<std::string> errors;
    g_regmap = std::make_shared<registry::map::RegMap>();
    if (!registry::map::load_dir(KILN_REGMAP_DIR, "server", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", g_regmap.get(), &errors)) return 1;
    try {
        world(); g_files.clear(); stage({});
        registry_live_world_begin(); apply_registry_files({});
        auto change = [](uint64_t owner, uint32_t point, int value) {
            uint64_t h = 0; RegistryLiveReply reply;
            check(registry_live_begin(owner, "sync", point, &h) == KILN_OK, "begin registry entry point");
            const auto ir = registry::json{{"ir", 1}, {"mod", "sync"}, {"stage", "alter"}, {"edits", registry::json::array({
                {{"registry", "items"}, {"select", "game:material_t1_wood_logs"}, {"field", "maxStack"}, {"op", value}},
                {{"registry", "recipes"}, {"select", "game:plank_recipe"}, {"field", "duration"}, {"op", value / 10.0}}
            })}}.dump();
            check(registry_live_submit(owner, h, ir, &reply, nullptr) == KILN_OK, "submit registry edits");
            check(registry_live_commit(owner, h, &reply) == KILN_OK && reply.outcome == RegistryLiveOutcome::Applied, "commit registry edits");
            return h;
        };
        auto snapshot = [](int value) {
            uint32_t fields = 0;
            const auto doc = registry::json::parse(export_changes_json(0, &fields));
            check(fields == 2, "both registry fields exported");
            check(doc["items"]["0x00000111"]["maxStackSize"] == value, "registry item layer reaches native sync export");
            check(doc["recipes"]["0x00000333"]["craftingDuration"] == value / 10.0, "registry recipe layer reaches native sync export");
        };
        auto first = change(77, 2, 90); // WorldLoad
        snapshot(90);
        auto later = change(88, 3, 120); // HotReload, client already joined
        snapshot(120);
        RegistryLiveReply reply;
        check(registry_live_revert(88, later, &reply) == KILN_OK, "remove later edit"); snapshot(90);
        check(registry_live_revert(77, first, &reply) == KILN_OK, "remove original edit");
        uint32_t fields = 99;
        auto empty = registry::json::parse(export_changes_json(0, &fields));
        check(fields == 0 && !empty.contains("items") && !empty.contains("recipes"), "removed registry edits leave no exported residue");
        std::printf("registry-sync-test: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) { std::printf("registry-sync-test: FAIL %s (%d checks passed)\n", e.what(), checks); return 1; }
}
