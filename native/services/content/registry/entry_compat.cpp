// SPDX-License-Identifier: MIT
#include "entry_compat.h"
#include <algorithm>
namespace kc::registry {
namespace {
bool guid_text(const std::string& value) {
    if (value.size() != 36) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? value[i] != '-' : !((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}
}
bool pin_entry(const std::optional<std::string>& stored, const std::string& mod, const std::string& id,
               const std::string& registry, const std::string& guid, const std::string& base_guid,
               const std::function<bool(const std::string&)>& write, std::string* why) {
    auto fail = [&](const std::string& reason) { *why = "entry compatibility: " + reason; return false; };
    std::string invalid;
    if (!mod_id_ok(mod, &invalid) || !id.starts_with(mod + ":") || id.size() <= mod.size() + 1 ||
        id.size() > 256 || (registry != "items" && registry != "recipes") || !guid_text(guid) || !guid_text(base_guid)) return fail("invalid stable entry identity");
    if (!std::all_of(id.begin() + static_cast<ptrdiff_t>(mod.size() + 1), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        })) return fail("invalid stable entry identity");
    if (stored && stored->size() > 32u * 1024 * 1024) return fail("record is oversized; this entry was refused");
    json document = stored ? json::parse(*stored, nullptr, false) : json{{"format", 1}, {"mods", json::object()}};
    if (!document.is_object() || !document.contains("format") || !document["format"].is_number_integer() || document["format"] != 1 ||
        !document.contains("mods") || !document["mods"].is_object()) return fail("record is corrupt or from an unsupported format; this entry was refused");
    auto& mods = document["mods"];
    if (mods.contains(mod) && !mods[mod].is_object()) return fail("mod record is corrupt; this entry was refused");
    if (!mods.contains(mod)) mods[mod] = json::object();
    auto& entries = mods[mod];
    if (entries.contains(id) && !entries[id].is_object()) return fail(id + " record is corrupt; this entry was refused");
    if (!entries.contains(id)) entries[id] = json::object();
    auto& kinds = entries[id];
    if (kinds.contains(registry)) {
        const auto& row = kinds[registry];
        if (!row.is_object() || !row.contains("guid") || !row["guid"].is_string() || !row.contains("baseGuid") || !row["baseGuid"].is_string() ||
            !guid_text(row["guid"].get<std::string>()) || !guid_text(row["baseGuid"].get<std::string>())) return fail(id + " record is corrupt; this entry was refused");
        if (row["guid"] != guid || row["baseGuid"] != base_guid) return fail(id + " cannot change GUID or structural game base; use a new Kiln id");
        return true;
    }
    kinds[registry] = {{"guid", guid}, {"baseGuid", base_guid}};
    if (!write || !write(document.dump())) return fail("atomic record write failed; this entry was refused before publication");
    return true;
}
} // namespace kc::registry
