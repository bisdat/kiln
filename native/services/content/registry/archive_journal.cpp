// SPDX-License-Identifier: MIT
// kiln.content registry stage: archive-writer attribution from kiln-eml's change journal (archive_journal.h).
// Reads the documented record (schema/sfi/eml-journal/startup-journal.schema.json, format 1); no kiln-eml code.
#include "archive_journal.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace kc::registry {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A resource key's GUID: 8-4-4-4-12 hex digits, as the game prints it. false: not one.
bool guid_of(const json& key, std::string* out) {
    if (!key.is_object() || !key.contains("guid") || !key["guid"].is_string()) return false;
    std::string g = lower(key["guid"].get<std::string>());
    if (g.size() != 36) return false;
    for (size_t i = 0; i < g.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? g[i] != '-' : !std::isxdigit(static_cast<unsigned char>(g[i]))) return false;
    }
    *out = std::move(g);
    return true;
}

// One package (or the unattributed bucket): its written and created resources, each GUID once. false + *why: malformed.
bool changes_of(const json& c, ArchiveJournal::Package* p, std::string* why) {
    if (!c.is_object()) return *why = "a package is not an object", false;
    if (c.contains("truncated") && c["truncated"].is_boolean()) p->truncated = c["truncated"].get<bool>();
    std::set<std::string> seen;
    for (const char* list : {"changed", "created"}) {
        if (!c.contains(list) || !c[list].is_array()) return *why = std::string("a package has no \"") + list + "\" list", false;
        auto& out = std::string(list) == "changed" ? p->changed : p->created;
        for (const json& k : c[list]) {
            std::string g;
            if (!guid_of(k, &g)) return *why = std::string("a \"") + list + "\" row of " + (p->id.empty() ? "the unattributed bucket" : p->id) + " has no resource GUID", false;
            if (seen.insert(std::string(list) + g).second) out.push_back(std::move(g));
        }
    }
    return true;
}

ArchiveJournal unreadable(const std::string& why) {
    ArchiveJournal j;
    j.state = "unreadable";
    j.text = "kiln-eml's change journal is not a format-1 journal (" + why + "): no archive-writer package is named in this start";
    return j;
}

}  // namespace

ArchiveJournal parse_archive_journal(const std::string& text, const std::string& live_generation) {
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return unreadable("not JSON");
    if (!doc.contains("format") || doc["format"] != 1) return unreadable("format is not 1");
    if (!doc.contains("generation") || !doc["generation"].is_string()) return unreadable("no generation");
    ArchiveJournal j;
    const std::string generation = lower(doc["generation"].get<std::string>());
    if (live_generation.empty() || generation != lower(live_generation)) {
        j.state = "stale";
        j.text = "kiln-eml's change journal names another generation than the one this start reads (" + generation.substr(0, 12) +
                 " is not the live " + (live_generation.empty() ? std::string("one") : lower(live_generation).substr(0, 12)) +
                 "): left by an earlier start, it names no package in this one";
        return j;
    }
    if (doc.contains("dialect") && doc["dialect"].is_string()) j.dialect = doc["dialect"].get<std::string>();
    if (doc.contains("truncated") && doc["truncated"].is_boolean()) j.truncated = doc["truncated"].get<bool>();
    if (!doc.contains("packages") || !doc["packages"].is_array()) return unreadable("no packages");
    std::string why;
    for (const json& p : doc["packages"]) {
        ArchiveJournal::Package pkg;
        if (!p.is_object() || !p.contains("id") || !p["id"].is_string() || p["id"].get<std::string>().empty()) return unreadable("a package has no id");
        pkg.id = p["id"].get<std::string>();
        if (p.contains("version") && p["version"].is_string()) pkg.version = p["version"].get<std::string>();
        pkg.kind = p.contains("kind") && p["kind"] == "eml" ? "eml" : "shroudforge";
        if (!changes_of(p, &pkg, &why)) return unreadable(why);
        j.packages.push_back(std::move(pkg));
    }
    if (doc.contains("unattributed") && !changes_of(doc["unattributed"], &j.unattributed, &why)) return unreadable(why);
    j.state = "read";
    size_t rows = 0;
    for (const auto& p : j.packages) rows += p.changed.size() + p.created.size();
    j.text = "kiln-eml's change journal names " + std::to_string(j.packages.size()) + (j.packages.size() == 1 ? " package" : " packages") +
             " of the startup pass (" + std::to_string(rows) + " resources written or created)" +
             (j.truncated ? "; it is truncated, so a package past its bounds is not named everywhere it wrote" : "");
    return j;
}

size_t name_archive_writers(const ArchiveJournal& journal, const std::map<std::string, std::string>& ids, const std::set<std::string>& whole,
                            BaseModel* base) {
    if (journal.state != "read" || !base) return 0;
    // GUID -> the entries that are that resource (one per registry it is an entry of); an element of a shared resource is
    // not one (its GUID member is no resource key)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> by_guid;
    for (const auto& [key, guid] : ids) {
        const size_t bar = key.find('|');
        if (bar == std::string::npos || guid.empty() || !whole.count(key.substr(0, bar))) continue;
        by_guid[lower(guid)].emplace_back(key.substr(0, bar), key.substr(bar + 1));
    }
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> names;
    auto note = [&](const ArchiveJournal::Package& p) {
        const std::string who = archive_writer_who(p.kind, p.id);
        for (const auto* list : {&p.created, &p.changed})
            for (const std::string& g : *list) {
                const auto it = by_guid.find(g);
                if (it == by_guid.end()) continue;
                for (const auto& entry : it->second) {
                    auto& v = names[entry];
                    if (std::find(v.begin(), v.end(), who) == v.end()) v.push_back(who);
                }
            }
    };
    note(journal.unattributed);
    for (const auto& p : journal.packages) note(p);
    size_t named = 0;
    for (auto& [reg, entries] : base->registries)
        for (BaseModel::Entry& e : entries) {
            const auto it = names.find({reg, e.id});
            if (it == names.end()) continue;
            e.archive = it->second;
            ++named;
        }
    return named;
}

}  // namespace kc::registry
