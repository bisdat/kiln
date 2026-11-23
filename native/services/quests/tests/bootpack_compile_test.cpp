// SPDX-License-Identifier: MIT
// Real compile failures at the per-mod boundary; reuse the synthetic resource store, not the fake compiler.
#define main memory_regression_main
#include "bootpack_memory_test.cpp"
#undef main

namespace {
ModInput knowledge_mod(const char* id) {
    ModInput mod;
    mod.id = id;
    mod.documents = {ojson{{"knowledge", {{"seen", {{"scope", "both"}}}}}}};
    return mod;
}
void missing_template_refuses_only_its_mod() {
    Store store;
    const auto backend = store.backend();
    auto first = knowledge_mod("a-good");
    auto missing = knowledge_mod("b-missing");
    missing.documents.push_back(ojson::parse(R"({"npcs":{"helper":{"template":"12345678-1234-5678-9234-123456789abc","clone":true}}})"));
    auto after = knowledge_mod("c-after");
    const auto result = bootpack::build({first, missing, after}, backend);
    CHECK(!result.stopped);
    CHECK(result.applied.size() == 2);
    std::vector<std::string> committed;
    for (const auto& applied : result.applied) committed.push_back(applied.mod);
    CHECK(committed == (std::vector<std::string>{"a-good", "c-after"}));
    CHECK(result.errors.size() == 1);
    if (!result.errors.empty()) {
        CHECK(result.errors.front().find("b-missing") != std::string::npos);
        CHECK(result.errors.front().find("template 12345678-1234-5678-9234-123456789abc") != std::string::npos);
    }
    CHECK(store.layers.size() == 2);
    if (!store.layers.empty()) {
        const auto& bytes = store.layers.back().bytes;
        for (const auto* id : {"a-good", "c-after"})
            CHECK(knowledge_find(bytes.data(), bytes.size(), false, derive_id(id, "know", "seen")) != nullptr);
        CHECK(knowledge_find(bytes.data(), bytes.size(), false, derive_id("b-missing", "know", "seen")) == nullptr);
    }
}
void shared_reads_still_stop_everything() {
    for (const auto& [type, resource] : {std::pair{kTypeKnowledge, kGuidKnowledge},
                                      std::pair{kTypeJournal, kGuidJournal}, std::pair{kTypeNpcs, kGuidNpcs}}) {
        Store store;
        auto backend = store.backend();
        const auto read = backend.game.read;
        backend.game.read = [=](const Guid& guid, uint32_t requested, const uint8_t** p, size_t* n) {
            return requested != type && read(guid, requested, p, n);
        };
        auto mod = knowledge_mod("shared");
        if (type == kTypeJournal)
            mod.documents.push_back(ojson::parse(R"({"quests":{"q":{"name":"Quest","steps":[{"id":"s","text":"Step"}]}}})"));
        if (type == kTypeNpcs)
            mod.documents.push_back(ojson::parse(R"({"npcs":{"helper":{"template":"12345678-1234-5678-9234-123456789abc","clone":true,"roster":{}}}})"));
        const auto result = bootpack::build({mod, knowledge_mod("after-shared")}, backend);
        CHECK(result.stopped);
        CHECK(result.stopped_mods == (std::vector<std::string>{"shared"}));
        CHECK(result.reason.find(resource) != std::string::npos);
        CHECK(result.applied.empty() && store.layers.empty() && store.next_owner == 0);
    }
}
}
int main() {
    missing_template_refuses_only_its_mod();
    shared_reads_still_stop_everything();
    std::printf("bootpack compile: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
