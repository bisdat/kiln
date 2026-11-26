// SPDX-License-Identifier: MIT
// kiln.content registry stage: which archive-writer package wrote which game entry before Kiln (commitment 7, D1).
//
// kiln-eml keeps a change journal of its kiln-mode ShroudForge-dialect startup pass beside its ownership receipt:
// <game>/kiln/eml/ownership/<stem>.change-journal.json (schema/sfi/eml-journal/startup-journal.schema.json, format 1).
// Per package of the committed attempt, in run order (ShroudForge packages and EML packages the pass ran), the archive
// resources its entrypoint wrote and the ones it created, by resource key (GUID, type hash, part); never a value. This
// reader takes that record (Kiln's own code over the documented file format; nothing of kiln-eml's source) and names each
// package on the registry entries whose resource it lists, so the report says "<package> (ShroudForge) wrote this before
// Kiln's edit; Kiln's edit wins (D1)" instead of nothing.
//
// What it can and cannot say:
//  - the journal is per resource: a package is named on every field of an entry whose resource it wrote, whatever member
//    it changed. "Wrote" means written, not changed (writing a field's current value lists the resource too). The journal
//    can over-name a writer, never miss one, within its bounds;
//  - a journal is used only when its generation is the start's live generation (the kiln-mode startup outcome's
//    archiveSha256): a writer that ignores the receipt (hosted ShroudForge, another loader) can leave one behind that
//    names another generation. Such a journal is "stale" and names nobody;
//  - only kiln-mode starts write one. Hosted or installed ShroudForge, kiln-eml's own EML dialect and other loaders keep
//    none: their pregame values are in the base and the report says no journal named them (Journal::state).
// Plain data: no file, no game memory, no lock.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "compose.h"

namespace kc::registry {

struct ArchiveJournal {
    // read: this start's journal, bound to its live generation; none: this start has none (the text says why); stale: one
    // for another generation; unreadable: the file is not a format-1 journal (the text says what is wrong).
    std::string state = "none";
    std::string text;
    std::string dialect;  // "shroudforge"
    bool truncated = false;
    struct Package {
        std::string id, version, kind;  // kind "shroudforge" | "eml"
        std::vector<std::string> changed, created;  // resource GUIDs, lower case, as the game prints them
        bool truncated = false;
    };
    std::vector<Package> packages;  // in run order
    Package unattributed;           // id empty: writes made while no entrypoint ran
};

// The journal's text against the generation this start's archive writer published (lower-case hex, 64 digits). Never
// throws: a malformed file is state "unreadable" with the reason.
ArchiveJournal parse_archive_journal(const std::string& text, const std::string& live_generation);

// What the report calls a package: "shroudforge:<id>" / "eml:<id>"; "kiln-eml:unattributed" for the unattributed bucket.
inline std::string archive_writer_who(const std::string& kind, const std::string& id) {
    return id.empty() ? std::string("kiln-eml:unattributed") : (kind == "eml" ? "eml:" : "shroudforge:") + id;
}
// For messages: "<id> (ShroudForge package)", "<id> (EML package)", "a startup write no package made (kiln-eml)".
inline std::string archive_writer_label(const std::string& who) {
    if (who == "kiln-eml:unattributed") return "a startup write no package made (kiln-eml)";
    if (who.rfind("eml:", 0) == 0) return who.substr(4) + " (EML package)";
    if (who.rfind("shroudforge:", 0) == 0) return who.substr(12) + " (ShroudForge package)";
    return who;
}

// Names the journal's packages on the base's game entries: every entry whose GUID (ids: "<registry>|<kiln id>" -> GUID
// text, the model's id table) a package wrote or created gets that package, in run order (BaseModel::Entry::archive).
// Returns how many entries were named.
size_t name_archive_writers(const ArchiveJournal& journal, const std::map<std::string, std::string>& ids, BaseModel* base);

}  // namespace kc::registry
