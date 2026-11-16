// SPDX-License-Identifier: MIT
// <game>/kiln/kiln.json (all keys optional):
//   { "policy": "verified-only" | "any", "clr": true, "plugins": true, "log_level": "info",
//     "native": { "policy": "always" | "verified-only" | "never" },
//     "preview": true | ["<capability>", ...],
//     "server": { ...same keys... }, "client": { ...same keys... } }
// The "server" / "client" section, when present, overrides the top-level keys on that side (apply_side, once the
// exe's identity is known), so one kiln.json can say e.g. verified-only for servers and any for a test client.
// policy verified-only: hooks are installed only when kiln/sigdb/builds/<side>/<exe sha256>.json says
// "verified": true. Otherwise Kiln stays in "safe idle": it resolves and reports, and the game runs vanilla.
// native.policy (native mods, docs/kiln/native-mods.md): always (default: they load in safe idle too), verified-only
// (only on a verified build), never.
// preview: lifts the client gate (gates.h): true (or "all") for every capability that is held until its game test, or
// a list of their names. A side section's "preview" replaces the top-level one.
// "tasks": { "workers": 1..8, "long_running_global": 1..16, "watchdog_running_s": 5..600, "watchdog_queued_s": 5..600 }:
// kiln.tasks' pool (TasksConfig below); also in a side section.
// "boot": { "testRefuse": { "<mod id>": "<collector>" } }: labs only (policy any, ignored otherwise): kiln.boot hands that
// collector an unreadable document of that mod, so the collector refuses it and the mod is dropped from the boot generation
// (the forced-drop proof of chapter 08; a release never sets it).
// "boot": { "testFault": { "<mod id>": "content" } }: labs only, the same rules: the world refuses that mod's first boot item or
// recipe at registration (its late-fault proof: a required mod stops the start at that load, an optional one runs without it).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "common.h"

namespace kiln {

enum class NativePolicy { Always, VerifiedOnly, Never };

// kiln.json "tasks" (kiln.tasks' pool; docs/kiln/services/tasks.md): the global knobs only. The per-owner limits are
// part of the contract mods see (kiln_tasks.h) and are not configurable. An absent key keeps the value before it (the
// default, or the top-level one for a side section); a wrong type or a value out of its bound gives the default and
// one kiln.log warning naming the key and the bound.
struct TasksConfig {
    uint32_t workers = 0;              // 1..8; 0 = the default clamp(cores / 4, 1, 4)
    uint32_t long_running_global = 8;  // 1..16
    uint32_t watchdog_running_s = 30;  // 5..600
    uint32_t watchdog_queued_s = 30;   // 5..600
};

struct Config {
    bool policy_any = false;
    bool clr = true;
    bool plugins = true;
    int log_level = KILN_LOG_INFO;
    NativePolicy native_policy = NativePolicy::Always;
    bool preview_all = false;           // "preview": true
    std::vector<std::string> preview;   // "preview": [names]
    std::map<std::string, std::string> boot_test_refuse;  // "boot": {"testRefuse": {mod: collector}} (labs, policy any)
    std::map<std::string, std::string> boot_test_fault;   // "boot": {"testFault": {mod: collector}} (labs, policy any)
    TasksConfig tasks;                                    // "tasks": {...} (a side section overrides key by key)
};

const char* native_policy_name(NativePolicy p);  // "always" | "verified-only" | "never"

const Config& config();
void load_config();
void apply_side(uint32_t side);  // KILN_SIDE_*: applies the side's section of kiln.json
bool build_verified(uint32_t side, const std::string& sha256, std::string* note);

}  // namespace kiln
