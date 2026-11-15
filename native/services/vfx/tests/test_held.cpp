// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdio>
#include "../held.h"
#ifdef HELD_COOK_TEST
#include "../cook/vfxcook.h"
#endif

int main() {
    unsigned checks = 0;
    // Release decision: clients keep ordinary files in on_ready, independent of the gate.
    for (const bool held : {false, true}) {
        assert(!kvx::boot_builds_in_prepare(KILN_SIDE_CLIENT, held));
        assert(kvx::boot_builds_in_prepare(KILN_SIDE_SERVER, held) == !held);
        checks += 2;
    }
    const auto run = [&](bool held, const std::vector<kvx::BuildFailure>& failures, bool required, bool excluded, bool refuses) {
        const std::string json = std::string(R"({"owners":[{"id":"mod","owner":1,"required":)") +
            (required ? "true" : "false") + R"(}],"excluded":)" + (excluded ? R"(["mod"])" : "[]") + "}";
        KilnBootPrepareInputV1 in{};
        in.size = sizeof in;
        in.canonical_ir_json = {json.data(), json.size()};
        KilnBootPreparedV1 out{};
        KilnBootIssueV1 issue{};
        const bool refused = kvx::held_build_refusal(held, failures, kiln::boot::parse(&in), &out, &issue);
        assert(refused == refuses);
        if (refused) {
            assert(out.ok == 0 && out.plan == nullptr);
            assert(std::string(issue.mod_id) == "mod");
            assert(std::string(issue.kind) == failures.front().kind);
            assert(std::string(issue.key) == failures.front().key);
            assert(std::string(issue.reason) == failures.front().reason);
        } else {
            kiln::boot::prepared(&out, &checks, nullptr);
            assert(out.ok == 1);
        }
        ++checks;
    };
    // One valid file contributes no failure; the broken file must still refuse the required participant.
    const std::vector<kvx::BuildFailure> broken{{"mod", "vfx", "blocks/broken.json", "invalid emitter"}};
    for (const bool held : {false, true})
        run(!kvx::boot_builds_in_prepare(KILN_SIDE_CLIENT, held), broken, true, false, true);
    run(true, {}, true, false, false); // only valid files
    run(true, broken, false, false, false); // optional boot mod
    run(false, broken, true, false, false); // no on_ready failure collection
    run(true, broken, true, true, false); // excluded participant on retry
    run(true, {{"nonboot", "vfx", "blocks/broken.json", "invalid emitter"}}, true, false, false);
    run(true, {{"mod", "vfx", "blocks/material.json", "invalid texture"}}, true, false, true);
    run(true, {{"mod", "vfx", "blocks/valid.json", "particle layout mismatch"}}, true, false, true);
    run(true, {{"mod", "vfx", "blocks/generated/", "file limit exceeded"}}, true, false, true);
#ifdef HELD_COOK_TEST
    kvb::ModInput mod;
    mod.id = "mod";
    mod.side = "client";
    mod.clients = "mod";
    mod.documents.emplace_back("blocks/valid.json", R"({"format":1,"vfx":{"ok":{"particles":[{"burst":1}]}}})");
    const auto valid = kvb::cook_mods({mod}, {});
    assert(valid.errors.empty() && valid.blocks.size() == 1);
    std::vector<kvx::BuildFailure> failures;
    kvx::record_cook_failures(&failures, valid.mod_errors);
    run(true, failures, true, false, false);
    mod.documents.emplace_back("blocks/broken.json", R"({"format":1,"vfx":{"bad":{"particles":"broken"}}})");
    const auto partial = kvb::cook_mods({mod}, {});
    assert(!partial.errors.empty() && partial.blocks.size() == 1);
    kvx::record_cook_failures(&failures, partial.mod_errors);
    assert(!failures.empty());
    for (const auto& f : failures) {
        if (f.key != "blocks/broken.json") std::fprintf(stderr, "key=%s reason=%s\n", f.key.c_str(), f.reason.c_str());
        assert(f.key == "blocks/broken.json");
    }
    for (const bool held : {false, true})
        run(!kvx::boot_builds_in_prepare(KILN_SIDE_CLIENT, held), failures, true, false, true);
    run(true, failures, false, false, false);
    run(false, failures, true, false, false);
#endif
    std::printf("held boot: %u cases passed\n", checks);
}
