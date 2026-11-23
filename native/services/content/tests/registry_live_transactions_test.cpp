// SPDX-License-Identifier: MIT
// Reuse the real store/live backend fixture; this executable adds transaction/entry-point cases, not another mock backend.
#define main registry_live_baseline_main
#include "registry_live_test.cpp"
#undef main
#include "services/kiln_registry_live.h"

namespace {
std::string doc(const std::string& mod, const std::string& edits) {
    return "{\"ir\":1,\"mod\":\"" + mod + "\",\"stage\":\"alter\",\"edits\":" + edits + "}";
}
uint64_t change(KilnOwner owner, const std::string& mod, const std::string& edits, uint32_t point = 4) {
    uint64_t h = 0; RegistryLiveReply reply;
    check(registry_live_begin(owner, mod, point, &h) == KILN_OK, "begin");
    check(registry_live_submit(owner, h, doc(mod, edits), &reply, nullptr) == KILN_OK, "submit");
    return h;
}
const std::string wood = R"("registry":"items","select":"game:material_t1_wood_logs","field":"maxStack",)";
void fresh_live() { world(); g_files.clear(); stage({}); registry_live_world_begin(); apply_registry_files({}); }

void packs_code_and_out_of_order_revert() {
    fresh_live(); put_pack(WOOD_AT, 20); std::string err; store_flush(&err);
    RegistryLiveReply reply;
    auto a = change(71, "a", "[{" + wood + R"("op":{"mul":3}}])");
    registry_live_commit(71, a, &reply);
    check(at<uint32_t>(WOOD_AT) == 60, "relative live edit composes on old pack");
    auto b = change(72, "b", "[{" + wood + R"("op":{"mul":2}}])");
    registry_live_commit(72, b, &reply);
    check(at<uint32_t>(WOOD_AT) == 120, "later transaction composes");
    registry_live_revert(71, a, &reply);
    check(at<uint32_t>(WOOD_AT) == 40, "reverting an earlier handle keeps the later multiplier");
    uint8_t bytes[4]; uint32_t code = 333; std::memcpy(bytes, &code, 4);
    store_put(CODE, next_code_seq(), KILN_CONTENT_ITEMS, WOOD, 0, -1, A(WOOD_AT), 4, bytes, "code"); store_flush(&err);
    registry_live_revert(72, b, &reply);
    check(at<uint32_t>(WOOD_AT) == 333, "revert never rolls back another code owner");
    store_drop_owner(CODE); store_flush(&err);
    check(at<uint32_t>(WOOD_AT) == 20, "old pack remains under every reverted transaction");
}
void unrelated_pack_binding_failure_isolated() {
    world();
    g_files = {{"p/content/p.json", R"({"registry":{"alter":[{"registry":"items","select":"game:material_t1_wood_logs","field":"manaCost","op":2}]}})"}};
    const auto files = files_of({{"p", "p/content/p.json"}});
    stage(files); registry_live_world_begin(); apply_registry_files(files);
    RegistryLiveReply reply;
    const auto h = change(71, "a", "[{" + wood + R"("op":{"mul":3}}])");
    check(registry_live_commit(71, h, &reply) == KILN_OK, "independent commit call succeeds");
    check(reply.outcome == RegistryLiveOutcome::Applied && reply.accepted == 1 && at<uint32_t>(WOOD_AT) == 30,
          "an unrelated pack's unbindable edit does not refuse this live change");
    check(reported("the item has no such configuration"), "the unrelated binding refusal remains visible in the report");
}
void unrelated_live_binding_failure_isolated() {
    fresh_live(); RegistryLiveReply reply;
    const auto a = change(71, "a", R"([{"registry":"items","select":"game:sword_t2","field":"manaCost","op":20}])");
    registry_live_commit(71, a, &reply);
    check(reply.outcome == RegistryLiveOutcome::Applied, "first live change initially binds");
    reg(KILN_CONTENT_ITEMS).by_id.erase(SWORD); // the earlier change's entry no longer binds in this world
    const auto b = change(72, "b", "[{" + wood + R"("op":{"mul":3}}])");
    check(registry_live_commit(72, b, &reply) == KILN_OK, "second independent commit call succeeds");
    check(reply.outcome == RegistryLiveOutcome::Applied && reply.accepted == 1 && at<uint32_t>(WOOD_AT) == 30,
          "an earlier live change's unbindable edit does not refuse another owner");
    check(reported("the entry is not in this world"), "the earlier change's binding refusal remains visible");
}
void pending_binding_failure_is_atomic() {
    world(); g_files.clear(); stage({});
    RegistryLiveReply reply;
    const auto h = change(71, "a", "[{" + wood + R"("op":75},{"registry":"items","select":"game:material_t1_wood_logs","field":"manaCost","op":2}])", 1);
    check(registry_live_commit(71, h, &reply) == KILN_OK && reply.outcome == RegistryLiveOutcome::PendingWorld && reply.accepted == 2,
          "the complete pre-world change waits for live binding");
    registry_live_world_begin(); apply_registry_files({});
    check(at<uint32_t>(WOOD_AT) == 10, "a pending change with a binding failure applies none of its fields");
    check(registry_live_status().active == 0, "the failed pending change is not retained as half applied");
    check(reported("not applied in this world"), "the failed pending change has a named world-application refusal");
    check(registry_live_revert(71, h, &reply) == KILN_ERR_NOT_FOUND, "a rejected pending handle is consumed");
}
void carried_binding_failure_is_atomic() {
    fresh_live(); RegistryLiveReply reply;
    const auto h = change(71, "a", "[{" + wood + R"("op":75},{"registry":"items","select":"game:sword_t2","field":"manaCost","op":20}])", 1);
    registry_live_commit(71, h, &reply);
    check(reply.outcome == RegistryLiveOutcome::Applied && at<uint32_t>(WOOD_AT) == 75, "the process change binds in its first world");
    registry_live_world_stop(); std::string error; store_reset(true, &error); world();
    reg(KILN_CONTENT_ITEMS).by_id.erase(SWORD);
    registry_live_world_begin(); apply_registry_files({});
    check(at<uint32_t>(WOOD_AT) == 10 && registry_live_status().active == 0,
          "a process change cannot carry only half its edits into the next world");
    check(reported("not applied in this world"), "the failed carry-over is reported by mod and change");
}
void reapplied_binding_failure_is_atomic() {
    fresh_live(); RegistryLiveReply reply;
    const auto h = change(71, "a", "[{" + wood + R"("op":75},{"registry":"items","select":"game:sword_t2","field":"manaCost","op":20}])", 1);
    registry_live_commit(71, h, &reply);
    check(at<uint32_t>(WOOD_AT) == 75, "the change is initially fully applied");
    reg(KILN_CONTENT_ITEMS).by_id.erase(SWORD);
    apply_registry_files({});
    check(at<uint32_t>(WOOD_AT) == 10 && registry_live_status().active == 0,
          "a pack application cannot leave half a now-unbindable retained change");
    check(reported("not applied in this world"), "the reapplication refusal remains visible");
}
void pending_evaluation_failure_is_atomic() {
    world(); g_files.clear(); stage({}); RegistryLiveReply reply;
    const auto h = change(71, "a", "[{" + wood + R"("op":75},{"registry":"items","select":"game:sword_t2","field":"manaCost","op":{"mul":1000}}])", 1);
    registry_live_commit(71, h, &reply);
    check(reply.outcome == RegistryLiveOutcome::PendingWorld && reply.accepted == 2, "archive values admit the pending relative change");
    mem<float>(SWORD_AT + 0x40, 200.0f); mem<int32_t>(SWORD_AT + 0x44, 200);
    registry_live_world_begin(); apply_registry_files({});
    check(at<uint32_t>(WOOD_AT) == 10 && at<float>(SWORD_AT + 0x40) == 200.0f,
          "a runtime evaluation failure is preflighted before any pending field writes");
    check(registry_live_status().active == 0 && reported("not applied in this world"), "the failed runtime evaluation deactivates and reports the whole change");
}
void pending_write_failure_is_rolled_back() {
    world(); g_files.clear(); stage({}); RegistryLiveReply reply;
    const auto h = change(71, "a", "[{" + wood + R"("op":75},{"registry":"items","select":"game:sword_t2","field":"maxStack","op":9}])", 1);
    registry_live_commit(71, h, &reply);
    check(reply.outcome == RegistryLiveOutcome::PendingWorld, "the two-field change waits for a world");
    g_write_limits = {1}; // one field really writes, then the second write fails; rollback is allowed to write normally
    registry_live_world_begin(); apply_registry_files({});
    check(at<uint32_t>(WOOD_AT) == 10 && at<uint32_t>(SWORD_AT) == 0,
          "a partial pending write failure restores the fields that already wrote");
    check(registry_live_status().active == 0 && reported("not applied in this world"), "a failed pending write is deactivated and reported");
}
void claims_and_binding_failure() {
    fresh_live(); RegistryLiveReply reply;
    auto a = change(71, "a", "[{" + wood + R"("op":50,"exclusive":true}])"); registry_live_commit(71, a, &reply);
    auto b = change(72, "b", "[{" + wood + R"("op":70}])"); registry_live_commit(72, b, &reply);
    check(reply.outcome == RegistryLiveOutcome::Refused && at<uint32_t>(WOOD_AT) == 50, "exclusive claims survive between entry points");
    auto partial = change(74, "partial", "[{" + wood + R"("op":70},{"registry":"loot","select":"game:material_t1_wood_logs","field":"stack.max","op":7}])");
    registry_live_commit(74, partial, &reply);
    check(reply.outcome == RegistryLiveOutcome::Applied && reply.accepted == 1 && reply.refused == 1, "only the nonconflicting edit is accepted");
    registry_live_revert(71, a, &reply);
    check(at<uint32_t>(WOOD_AT) == 10, "a refused edit never activates later when its conflicting owner leaves");
    registry_live_revert(74, partial, &reply);
    auto c = change(73, "c", "[{" + wood + R"("op":75},{"registry":"items","select":"game:material_t1_wood_logs","field":"manaCost","op":2}])");
    registry_live_commit(73, c, &reply);
    check(reply.outcome == RegistryLiveOutcome::Refused && at<uint32_t>(WOOD_AT) == 10, "binding failure rolls the candidate's other fields back");
}
void mapped_reads_have_no_write_side_effect() {
    fresh_live(); put_pack(WOOD_AT, 20); std::string err; store_flush(&err);
    const auto before = store_generation(); const auto owners = store_owners(); std::string value;
    check(registry_live_read("items", "game:material_t1_wood_logs", "maxStack", &value) == KILN_OK && value == "20", "read actual old-pack value, not cached archive value");
    check(registry_live_read("recipes", "game:plank_recipe", "inputs", &value) == KILN_OK && registry::json::parse(value)[0]["count"] == 2, "read mapped recipe list");
    check(registry_live_read("items", "game:sword_t2", "manaCost", &value) == KILN_OK && registry::json::parse(value) == 10, "read mapped cost");
    check(registry_live_read("recipes", "game:plank_recipe", "inputs[0].count", &value) == KILN_OK && value == "2", "read one mapped list leaf");
    check(registry_live_read("recipes", "game:plank_recipe", "inputs[].count", &value) == KILN_OK && value == "[2]", "read a schema list-leaf column");
    check(registry_live_read("recipes", "game:plank_recipe", "inputs[99999999999999999999999999999999].count", &value) == KILN_ERR_INVALID_ARGUMENT, "index parsing is bounded before overflow");
    check(store_generation() == before && store_owners() == owners, "reads create no layers or writes");
}
void world_scope_and_admission() {
    fresh_live(); RegistryLiveReply reply;
    auto process = change(71, "a", "[{" + wood + R"("op":{"mul":3}}])", 1); registry_live_commit(71, process, &reply);
    auto scoped = change(72, "b", "[{" + wood + R"("op":70}])", 2); registry_live_commit(72, scoped, &reply);
    check(at<uint32_t>(WOOD_AT) == 70, "world-local entry point wins");
    const auto token = registry_live_status().world;
    registry_live_world_stop(); std::string err; store_reset(true, &err); world(); registry_live_world_begin(); apply_registry_files({});
    check(registry_live_status().world != token && at<uint32_t>(WOOD_AT) == 30, "new world expires only world-local changes");
    check(registry_live_revert(72, scoped, &reply) == KILN_ERR_NOT_FOUND, "old world handle never aliases a new world");
    uint64_t h = 0;
    check(registry_live_begin(0, "a", 1, &h) == KILN_ERR_INVALID_ARGUMENT, "owner zero refused");
    check(registry_live_begin(71, "a", 99, &h) == KILN_ERR_INVALID_ARGUMENT, "unknown entry point refused");
    registry_live_begin(71, "a", 4, &h);
    check(registry_live_submit(71, h, doc("other", "[]"), &reply, nullptr) == KILN_ERR_INVALID_ARGUMENT, "batch cannot impersonate another mod");
    registry_live_revert(71, h, &reply); registry_live_revert(71, process, &reply);
}
void repeated_submit_numbers_are_distinct() {
    fresh_live(); RegistryLiveReply reply;
    auto h = change(71, "a", "[{" + wood + R"("op":30}])");
    registry_live_submit(71, h, doc("a", R"([{"registry":"items","select":"game:missing","field":"maxStack","op":70}])"), &reply, nullptr);
    registry_live_commit(71, h, &reply);
    check(reply.outcome == RegistryLiveOutcome::Applied && reply.accepted == 1 && reply.refused == 1 && at<uint32_t>(WOOD_AT) == 30,
          "a refused edit numbered zero in a second submit cannot erase the first submit's zero");
    registry_live_revert(71, h, &reply);
}
void restart_is_refused_at_every_live_point() {
    for (uint32_t point = 1; point <= 4; ++point) {
        fresh_live(); RegistryLiveReply reply;
        const auto h = change(71, "a", R"([{"registry":"balance","select":"game:balance","field":"levelCap","op":60}])", point);
        check(registry_live_commit(71, h, &reply) == KILN_OK && reply.outcome == RegistryLiveOutcome::RestartRequired && reply.accepted == 0,
              "each later entry point refuses restart-only edits with RestartRequired");
    }
}
void refused_edits_share_change_budget() {
    fresh_live(); RegistryLiveReply reply; uint64_t h = 0;
    check(registry_live_begin(71, "a", 4, &h) == KILN_OK, "begin bounded change");
    std::string edits = "[";
    for (int i = 0; i < 4096; ++i) {
        if (i) edits += ',';
        edits += R"({"registry":"balance","select":"game:balance","field":"levelCap","op":60})";
    }
    edits += ']';
    check(registry_live_submit(71, h, doc("a", edits), &reply, nullptr) == KILN_OK && reply.refused == 4096,
          "refused edits are retained in the change's budget");
    check(registry_live_submit(71, h, doc("a", "[{" + wood + R"("op":75}])"), &reply, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "the 4096-edit change limit includes prior refused edits");
    registry_live_revert(71, h, &reply);
}
void restart_is_per_edit_and_begin_is_bounded() {
    fresh_live(); RegistryLiveReply reply;
    auto h = change(71, "a", "[{" + wood + R"("op":75},{"registry":"balance","select":"game:balance","field":"levelCap","op":60}])", 3);
    registry_live_commit(71, h, &reply);
    check(reply.outcome == RegistryLiveOutcome::Applied && reply.refused == 1 && at<uint32_t>(WOOD_AT) == 75, "restart-only refusal does not silently write or discard mapped live edits");
    check(reported("RestartRequired"), "restart requirement is in registry.json");
    registry_live_revert(71, h, &reply);
    std::vector<uint64_t> drafts;
    for (int i = 0; i < 64; ++i) { uint64_t n = 0; check(registry_live_begin(71, "a", 4, &n) == KILN_OK, "draft slot"); drafts.push_back(n); }
    check(registry_live_begin(71, "a", 4, &h) == KILN_ERR_UNAVAILABLE, "draft bound is enforced");
    for (auto n : drafts) registry_live_revert(71, n, &reply);
}
}
int main() {
    std::vector<std::string> errs; g_regmap = std::make_shared<registry::map::RegMap>();
    if (!registry::map::load_dir(KILN_REGMAP_DIR, "server", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", g_regmap.get(), &errs)) return 2;
    int failed = 0;
    const std::pair<const char*, void(*)()> tests[] = {
        {"packs_code_and_out_of_order_revert", packs_code_and_out_of_order_revert},
        {"unrelated_pack_binding_failure_isolated", unrelated_pack_binding_failure_isolated},
        {"unrelated_live_binding_failure_isolated", unrelated_live_binding_failure_isolated},
        {"pending_binding_failure_is_atomic", pending_binding_failure_is_atomic},
        {"carried_binding_failure_is_atomic", carried_binding_failure_is_atomic},
        {"reapplied_binding_failure_is_atomic", reapplied_binding_failure_is_atomic},
        {"pending_evaluation_failure_is_atomic", pending_evaluation_failure_is_atomic},
        {"pending_write_failure_is_rolled_back", pending_write_failure_is_rolled_back},
        {"claims_and_binding_failure", claims_and_binding_failure},
        {"mapped_reads_have_no_write_side_effect", mapped_reads_have_no_write_side_effect},
        {"world_scope_and_admission", world_scope_and_admission},
        {"repeated_submit_numbers_are_distinct", repeated_submit_numbers_are_distinct},
        {"restart_is_refused_at_every_live_point", restart_is_refused_at_every_live_point},
        {"refused_edits_share_change_budget", refused_edits_share_change_budget},
        {"restart_is_per_edit_and_begin_is_bounded", restart_is_per_edit_and_begin_is_bounded}};
    for (auto [name, fn] : tests) try { fn(); std::printf("ok %s\n", name); } catch (const std::exception& e) { ++failed; std::printf("FAIL %s: %s\n", name, e.what()); }
    std::printf("registry live transactions: %d checks, %d failed groups\n", checks, failed);
    return failed ? 1 : 0;
}
