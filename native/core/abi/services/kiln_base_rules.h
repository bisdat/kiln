/* SPDX-License-Identifier: MIT */
/*
 * kiln.base_rules v1 (since 0.2.0-dev.31): the read side of Kiln's base rules. Published by kiln-content.dll
 * (gamedata_bootpatch.cpp, baserules.hpp). docs/kiln/services/build.md, "Base rules and base limits".
 *
 * A mod states in Kiln's own names how many Flame Altars a world may have and how large an altar's build zone is
 * (the "baseRules" section of a blocks file, or BaseRulesBlock staged in Mod.OnBoot). kiln.content composes every mod's rules into one
 * change of the game's balancing table, written after every mod's own resource patches as a raise-only pass: the count
 * per Flame level is the largest any rule asks for (never below the value as the patches left it, never above 16 by a
 * rule), each build-zone axis per altar level the largest too. It is written when the game's resource service comes up,
 * before the game reads the table, and holds for the run of the process.
 *
 * state() answers this process's table as JSON (NUL-terminated):
 *   {"table": true|false,          the balancing table was found and read ("reason" says why not)
 *    "reason": "...",
 *    "original":  {"altars": [count per Flame level 0..], "sizes": [[x, y, z] per altar level 0..]},
 *                                  the game's own values: read before any boot patch of this start (the served table
 *                                  when no boot patch of this start changed it)
 *    "effective": {"altars": [...], "sizes": [...]},   the table the game reads now (as kiln.assets serves it)
 *    "rules": [{"mod", "name", "minimumAltars", "extraAltars", "buildAreaPercent",
 *               "sizes": [{"altarLevel", "size": [x, y, z]}], "applied": true|false}],   in load order
 *    "composed": {"minimumAltars", "extraAltars", "buildAreaPercent", "sizes": [...]} | null,
 *    "applied": true|false,         Kiln's pass is in the served table ("fault" says why not when there are rules)
 *    "fault": "...",
 *    "patchedBy": ["mod", ...]}     mods whose own resource patches of the table were applied this start
 * A member the rule left out is absent (sizes: an empty list). Sizes are full sizes in metres (the game halves them into
 * the zone's half-extent).
 * Returns KILN_ERR_BUFFER_TOO_SMALL (with *out_len the length without the NUL) when cap is too small, KILN_ERR_UNAVAILABLE
 * when kiln.assets has no resources on this build.
 *
 * Threads: state() on the tick thread (it reads the served table, which is game memory).
 * Capabilities: build.base_rules (kiln.content's row: rules can be staged and are applied in this process),
 * build.base_limits (Kiln.Build's row: this table and the world's flame level can be read).
 */
#ifndef KILN_BASE_RULES_H
#define KILN_BASE_RULES_H

#include "../kiln_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_BASE_RULES_SERVICE "kiln.base_rules"
#define KILN_BASE_RULES_VERSION 1

typedef struct KilnBaseRulesApi {
    KilnServiceHeader header;
    KilnResult(KILN_CALL* state)(char* json, size_t cap, size_t* out_len);
} KilnBaseRulesApi;

#ifdef __cplusplus
}
static_assert(sizeof(KilnServiceHeader) == 8, "kiln_base_rules.h: KilnServiceHeader layout");
static_assert(offsetof(KilnBaseRulesApi, state) == 8, "kiln_base_rules.h: v1 layout");
static_assert(sizeof(KilnBaseRulesApi) == 8 + sizeof(void*), "kiln_base_rules.h: v1 size");
#endif
#endif
