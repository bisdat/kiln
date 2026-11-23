// SPDX-License-Identifier: MIT
#include "../registry/entry_compat.h"
#include <cstdio>
using namespace kc::registry;
int main() {
    int passed = 0, failed = 0;
    auto check = [&](bool yes, const char* name) { if (yes) ++passed; else { ++failed; std::printf("FAIL %s\n", name); } };
    std::optional<std::string> stored; std::string why; int writes = 0;
    const std::string guid = "12345678-1234-5678-9123-123456789abc", base = "22345678-1234-5678-9123-123456789abc";
    auto write = [&](const auto& text) { ++writes; stored = text; return true; };
    check(pin_entry(stored, "a", "a:logs", "items", guid, base, write, &why), "new stable entry pins in the install-wide compatibility record");
    check(stored.has_value() && writes == 1, "pin is durable before publication is allowed");
    check(pin_entry(stored, "a", "a:logs", "items", guid, base, write, &why) && writes == 1, "same structural identity survives restart without rewriting the record");
    check(!pin_entry(stored, "a", "a:logs", "items", guid, guid, write, &why) && writes == 1, "a stable id cannot switch structural game base");
    check(why.find("new Kiln id") != std::string::npos, "structural refusal tells authors to mint a new identity");
    check(!pin_entry(stored, "a", "a:logs", "items", base, base, write, &why), "a stable id cannot switch its derived GUID");
    check(!pin_entry(stored, "a", "other:logs", "items", guid, base, write, &why), "compatibility storage is bound to the mod namespace");
    check(!pin_entry(std::string("not JSON"), "a", "a:logs", "items", guid, base, write, &why), "corrupt whole records refuse each attempted entry, not overwrite evidence");
    check(!pin_entry(std::string(R"({"format":9,"mods":{}})"), "a", "a:logs", "items", guid, base, write, &why), "unknown compatibility formats fail closed");
    check(!pin_entry(std::nullopt, "a", "a:logs", "items", guid, base, [](const auto&) { return false; }, &why), "failed atomic storage does not authorize an add");
    if (stored) {
        auto doc = json::parse(*stored); doc["mods"]["a"]["a:broken"] = 3; stored = doc.dump();
        check(!pin_entry(stored, "a", "a:broken", "items", guid, base, write, &why), "a corrupt entry is refused on its own");
        check(pin_entry(stored, "a", "a:logs", "items", guid, base, write, &why), "an unrelated valid entry stays usable beside corrupt state");
        check(pin_entry(stored, "a", "a:recipe", "recipes", base, base, write, &why), "new unrelated identities can be pinned without discarding the corrupt row");
        check(json::parse(*stored)["mods"]["a"]["a:broken"] == 3, "writes preserve evidence for corrupt siblings");
    }
    std::printf("registry-entry-compat-test: %d passed, %d failed\n", passed, failed); return failed ? 1 : 0;
}
