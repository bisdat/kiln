// SPDX-License-Identifier: MIT
// The actual private content helper and its text shards, over the existing fake game/services.
#define main legacy_text_main
#include "text_test.cpp"
#undef main

namespace {
int boot_checks = 0;
#define CHECK(value) do { ++boot_checks; check((value), #value); } while (0)
KilnSessionInfo boot_session{};
KilnResult KILN_CALL session(KilnSessionInfo* out) { *out = boot_session; return KILN_OK; }
std::string result;
KilnResult call(uint32_t operation, KilnOwner owner, const std::string& text = "", uint32_t kind = 0) {
    char report[2048]{};
    size_t length = 0;
    const auto rc = kc::kc_nc_boot_pack_v1(operation, owner, kind, text.data(), text.size(), report, sizeof report, &length);
    result = report;
    return rc;
}
const kc::TextEntry* entry(uint32_t tag) {
    const auto* shard = kc::text_shard(tag).load();
    if (!shard) return nullptr;
    const auto found = shard->find(tag);
    return found == shard->end() ? nullptr : found->second;
}
std::string item(const char* guid, const char* name, const char* display) {
    return kc::json{{"guid", guid}, {"name", name}, {"base", "TestBase"}, {"displayName", display}}.dump();
}
}
int main() {
    using namespace kc::bootpack;
    fake::api.session = &session;
    kc::N.on = true;
    const auto definition = item("12345678-1234-5678-9234-123456789abc", "BootItem", "owned text");
    uint8_t guid[16]{}, tag_guid[16]{};
    kc::guid_parse("12345678-1234-5678-9234-123456789abc", guid);
    kc::derive_guid(guid, kc::kTextPurpose[0], tag_guid);
    const auto tag = kc::fnv1a(tag_guid, 16);
    kc::set_text(tag, "original");
    const auto* original = entry(tag);
    CHECK(call(Begin, 10, R"({"mod":"body-mod"})") == KILN_OK);
    CHECK(call(Define, 10, definition, KILN_CONTENT_ITEMS) == KILN_OK);
    CHECK(kc::g_defs.size() == 1);
    CHECK(kc::g_defs.front()->owner_label == "bootpack:body-mod");
    CHECK(kc::g_defs.front()->mod == "body-mod");
    CHECK(entry(tag) != original);
    const auto bad = item("22345678-1234-5678-9234-123456789abc", "invalid/name", "bad");
    CHECK(call(Define, 10, bad, KILN_CONTENT_ITEMS) != KILN_OK);
    kc::set_text(99, "independent writer");
    const auto* unrelated = entry(99);
    CHECK(call(Abort, 10) == KILN_OK);
    CHECK(kc::g_defs.empty());
    CHECK(entry(tag) == original);
    CHECK(entry(99) == unrelated);
    CHECK(call(Abort, 10) == KILN_OK);

    CHECK(call(Begin, 11, R"({"mod":"first"})") == KILN_OK);
    CHECK(call(Define, 11, definition, KILN_CONTENT_ITEMS) == KILN_OK);
    CHECK(call(Finish, 11) == KILN_OK);
    CHECK(call(Text, 11, R"({"tag":42,"texts":"closed"})") == KILN_ERR_UNAVAILABLE);
    CHECK(call(Begin, 12, R"({"mod":"second"})") == KILN_OK);
    CHECK(call(Define, 12, definition, KILN_CONTENT_ITEMS) != KILN_OK);
    CHECK(call(Abort, 12) == KILN_OK);
    CHECK(kc::g_defs.size() == 1 && kc::g_defs.front()->owner == 11);
    kc::g_defs.front()->error = "injected registry refusal";
    std::vector<kc::BootFailed> failed;
    kc::nc_boot_failed(&failed);
    CHECK(failed.size() == 1 && failed.front().mod == "first");
    CHECK(call(Abort, 11) == KILN_OK); // enclosing OnBoot can still withdraw a finished pack
    CHECK(kc::g_defs.empty() && entry(tag) == original);

    CHECK(call(Begin, 13, R"({"mod":"conflict"})") == KILN_OK);
    CHECK(call(Text, 13, R"({"tag":42,"texts":"owned"})") == KILN_OK);
    CHECK(call(Text, 13, R"({"tag":43,"texts":"new tag"})") == KILN_OK);
    const auto* added = entry(43);
    kc::set_text(42, "foreign writer");
    const auto* foreign = entry(42);
    CHECK(call(Abort, 13) == KILN_ERR_INTERNAL);
    CHECK(result.find("42") != std::string::npos);
    CHECK(entry(42) == foreign && entry(43) == added); // no shard was partly restored

    boot_session.sessions[KILN_WORLD_SERVER] = 1;
    CHECK(call(Begin, 14, R"({"mod":"too-late"})") == KILN_ERR_UNAVAILABLE);
    CHECK(!kc::g_boot_text_undo.count(14));
    std::printf("boot content: %d checks, %d failed\n", boot_checks, failures);
    return failures ? 1 : 0;
}
