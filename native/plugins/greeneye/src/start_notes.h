// SPDX-License-Identifier: MIT
// What one Greeneye mod asked of find and read while it started (kiln#157). Hollow Halls Progression said only
// "unsupported game signatures" and the report could not tell which signature or read had failed: kiln.log now says
// it for a mod that does not start, and names the reads that saw through another module's hook for one that does.
// No Windows dependencies: the host test checks the lines.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hot {

struct StartNotes {
    static constexpr size_t kKeep = 4; // patterns and places named in the line; the counts count them all
    unsigned finds = 0, missed = 0, unhooked = 0;
    std::vector<std::string> missedPatterns;
    std::vector<uint32_t> unhookedRvas;

    void Find(const char* pattern, bool found)
    {
        ++finds;
        if (found) return;
        ++missed;
        if (missedPatterns.size() >= kKeep) return;
        std::string p = pattern ? pattern : "(none)";
        if (p.size() > 72) p = p.substr(0, 69) + "...";
        missedPatterns.push_back(std::move(p));
    }
    void Read(uint32_t rva)
    {
        ++unhooked;
        if (unhookedRvas.size() < kKeep) unhookedRvas.push_back(rva);
    }

    // The line for kiln.log after the start, empty when there is nothing to say. `code`: where find's code came from.
    std::string Line(bool started, const std::string& code) const
    {
        std::string places;
        for (uint32_t rva : unhookedRvas) {
            char b[24];
            std::snprintf(b, sizeof b, "%s0x%x", places.empty() ? "" : ", ", rva);
            places += b;
        }
        if (unhooked > unhookedRvas.size()) places += ", ...";
        const std::string reads = unhooked ? std::to_string(unhooked) + (unhooked == 1 ? " read" : " reads") +
                                                 " returned the game's own bytes under another module's hook (rva " +
                                                 places + ")"
                                           : std::string();
        if (started) return reads;
        std::string line = "while starting it asked find for " + std::to_string(finds) +
                           (finds == 1 ? " signature" : " signatures") + " in the game's code (" + code + ")";
        if (missed) {
            line += "; not found: " + std::to_string(missed);
            for (size_t i = 0; i < missedPatterns.size(); ++i) line += (i ? ", \"" : " (\"") + missedPatterns[i] + "\"";
            line += missed > missedPatterns.size() ? ", ...)" : ")";
        } else if (finds) {
            line += ", and every one was found";
        }
        if (!reads.empty()) line += "; " + reads;
        if (!missed) line += "; so it declined for a reason of its own (see its lines above)";
        return line;
    }
};

} // namespace hot
