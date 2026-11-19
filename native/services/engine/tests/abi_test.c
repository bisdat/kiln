/* SPDX-License-Identifier: MIT */
#include "services/kiln_engine.h"
_Static_assert(sizeof(KilnEngineIdentity) == 192, "identity size");
_Static_assert(offsetof(KilnEngineIdentity, target) == 32 && offsetof(KilnEngineIdentity, sha256) == 64 &&
               offsetof(KilnEngineIdentity, profile_id) == 129, "identity strings");
_Static_assert(sizeof(KilnEngineGateStats) == 96 && offsetof(KilnEngineGateStats, hold_ns_max) == 72, "stats");
_Static_assert(sizeof(KilnEngineApi) == 24 && offsetof(KilnEngineApi, identity) == 8 && offsetof(KilnEngineApi, gate_stats) == 16,
               "kiln.engine v1 table");
