// SPDX-License-Identifier: MIT
// What kiln.knowledge says when it stops the game's start (start_stop.cpp refuse_start), per reason: the exit code,
// the state file, the kiln.log lines, kiln.boot's reason, the player's message and the state file's fields. Portable
// (no Windows, no Kiln API): tests/native_pack_test.cpp checks the texts on the host.
//
// Two reasons, both before any world loads, because a world loaded without the mods' native data drops their
// progress at its next save:
//  - Roster (package 17c): the mods' NPC roster cannot be held (KILN_KNOW_ROSTER_EXIT_CODE, roster-refused.json).
//  - Content (0.2.0-dev.29): the whole build of the mods' native quests, knowledge and NPCs failed, so nothing of it
//    is served (KILN_KNOW_CONTENT_EXIT_CODE, content-refused.json). One mod's own errors never stop the start: that
//    mod loads without its content (native_pack.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "kiln_knowledge.h"
#include "nlohmann/json.hpp"

namespace kiln::knowledge {

enum class StartStop : uint8_t { Roster, Content };

namespace stop {

struct Kind {
    uint32_t exit_code;
    const char* tag;    // the log lines' head ("roster", "native")
    const char* state;  // kiln/state/<state>; the next start moves it to <last>
    const char* last;
};

inline const Kind& kind(StartStop k) {
    static const Kind roster{KILN_KNOW_ROSTER_EXIT_CODE, "roster", "roster-refused.json", "roster-refused.last.json"};
    static const Kind content{KILN_KNOW_CONTENT_EXIT_CODE, "native", "content-refused.json", "content-refused.last.json"};
    return k == StartStop::Roster ? roster : content;
}

// What a Linux parent sees when the server runs under Wine: the low byte of the code.
inline uint32_t unix_status(StartStop k) { return kind(k).exit_code & 0xff; }

inline std::string list(const std::vector<std::string>& mods) {
    std::string s;
    for (const std::string& m : mods) s += (s.empty() ? "" : ", ") + m;
    return s.empty() ? "(none named)" : s;
}
inline std::string csv(const std::vector<std::string>& mods) {
    std::string s;
    for (const std::string& m : mods) s += (s.empty() ? "" : ",") + m;
    return s;
}

// What to do, one paragraph: in kiln.log, kiln.boot's reason and the state file.
inline std::string advice(StartStop k) {
    if (k == StartStop::Roster)
        return "Do not restart the game or the server: the next start stops the same way (a dedicated server under "
               "Wine exits with status 2; a supervisor must not restart on it). Fix the mod list: remove these mods, or "
               "run a Kiln and game build where the roster's second unlock page installs";
    return "Do not restart the game or the server in a loop: the next start stops the same way (a dedicated server "
           "under Wine exits with status " + std::to_string(unix_status(k)) + "; a supervisor must not restart on it). "
           "Remove or update the mods named here, then start again. If it still stops, send kiln/logs/kiln.log (the "
           "launcher's or kilnctl's support report has it) to the mods' authors or to Kiln";
}

// The head of the error line: kiln.log is searched for it before the process ends by itself (an older kiln.boot).
inline std::string head(StartStop k, unsigned long pid) {
    char b[96];
    std::snprintf(b, sizeof b, "%s: the start is stopped (pid %lu, exit code 0x%X): ", kind(k).tag, pid, kind(k).exit_code);
    return b;
}

// The error line in kiln.log (after head()).
inline std::string line(StartStop k, const std::string& why, const std::vector<std::string>& mods) {
    if (k == StartStop::Roster)
        return why + "; mods: " + list(mods) +
               ". No world was loaded and no save was opened: a world without these mods' NPCs and knowledge would drop "
               "their progress at its next save. " + advice(k) + " (kiln/state/roster-refused.json)";
    return "the mods' native quests, knowledge and NPCs could not be built at all (" + why + "); mods: " + list(mods) +
           ". No world was loaded and no save was opened: nothing of these mods' NPCs, quests and knowledge could be "
           "served, and a world loaded without them would drop their progress at its next save. " + advice(k) +
           " (kiln/state/content-refused.json)";
}

// kiln.boot's stop_start reason (its stop line, boot.json "boot"."stop"."reason", progress.json's reason).
inline std::string reason(StartStop k, const std::string& why, const std::vector<std::string>& mods) {
    if (k == StartStop::Roster) return "kiln.knowledge: " + why + "; mods: " + list(mods) + ". " + advice(k);
    return "kiln.knowledge: the mods' native quests, knowledge and NPCs could not be built at all (" + why + "); mods: " +
           list(mods) + ". " + advice(k);
}

// The player's message on a game client (it closes by itself after 60 s) and its title.
inline std::string box(StartStop k, const std::string& why, const std::vector<std::string>& mods) {
    if (k == StartStop::Roster)
        return "Kiln stopped the game's start.\n\n" + why + "\n\nMods: " + list(mods) +
               "\n\nNo world was loaded and no save was opened, so nothing of your progress was lost. Starting again stops "
               "the same way: remove these mods, or update Kiln.\n\nkiln/logs/kiln.log and kiln/state/roster-refused.json "
               "say more. This message closes by itself after 60 seconds.";
    return "Kiln stopped the game's start: the NPCs and quests of these mods could not be built.\n\n" + why + "\n\nMods: " +
           list(mods) +
           "\n\nNo world was loaded and no save was opened, so nothing of your progress was lost. Starting again stops "
           "the same way: remove or update these mods. If it keeps stopping, send kiln/logs/kiln.log to the mods' "
           "authors or to Kiln.\n\nkiln/logs/kiln.log and kiln/state/content-refused.json say more. This message closes "
           "by itself after 60 seconds.";
}
inline const char* title(StartStop k) {
    return k == StartStop::Roster ? "Kiln: the NPC roster is too long" : "Kiln: the mods' NPCs and quests could not be built";
}

// The error line when a world is present or has run in this process: nothing is stopped then (it could lose play).
inline std::string cannot_stop(StartStop k, const std::string& why, const std::vector<std::string>& mods) {
    if (k == StartStop::Roster)
        return "roster: the start could NOT be stopped: a world already ran in this process, and ending it now could lose "
               "play. These mods' native quests, knowledge and NPCs are not built; a world saved now drops their "
               "progress. Quit and fix the mod list";
    return "native: the start could NOT be stopped: a world already ran in this process, and ending it now could lose "
           "play. The mods' native quests, knowledge and NPCs are not served (" + why + "; mods: " + list(mods) +
           "); a world saved now drops their progress. Quit, then remove or update these mods";
}

// The line at the next start, when it moved the state file aside.
inline std::string previous(StartStop k) {
    return k == StartStop::Roster
               ? "roster: the previous start was stopped by the roster (kiln/state/roster-refused.last.json says why)"
               : "native: the previous start was stopped because the mods' native quests and NPCs could not be built "
                 "(kiln/state/content-refused.last.json says why)";
}

// kiln/state/<kind(k).state>: schema/state-roster-refused.schema.json, schema/state-content-refused.schema.json.
inline nlohmann::json state(StartStop k, const std::string& why, const std::vector<std::string>& mods, bool client, bool boot_stop,
                            uint32_t pid, const std::string& utc) {
    return {{"format", 1},
            {"refused", true},
            {"why", why},
            {"mods", mods},
            {"side", client ? "client" : "server"},
            {"exitCode", kind(k).exit_code},
            {"unixExitStatus", unix_status(k)},
            {"restart", false},
            {"advice", advice(k)},
            {"bootStop", boot_stop},
            {"pid", pid},
            {"utc", utc}};
}

}  // namespace stop
}  // namespace kiln::knowledge
