/* SPDX-License-Identifier: MIT
 * kiln.launch v1 (in-core service): which ShroudForge backend this start runs, decided once by the core and frozen
 * before any native mod, early plugin or service runs (docs/kiln/services/launch.md).
 *
 * Kiln-internal: the core and Kiln's own loaders read it; it is not in the SDK's headers and is no mod API. Its header
 * is plugin-visible ABI all the same: the loaders that need the selection
 * (kiln-sfi, kiln-shroudforge, kiln-eml) are plugins, and plugins cannot resolve a kiln.internal.* table.
 *
 * Getting the table: api->service({"kiln.launch", 11}, 1). The core answers it in every view and in every mode, early
 * plugins, native mods, the managed host and safe idle included, like kiln.profile. A null table means an older core:
 * such a core decides nothing, and every loader keeps the rules it had before this table. Nobody can publish the name.
 *
 * What it says, and what it never does:
 *  - One record for the life of the process (status). It is frozen at boot, before the first native mod of phase
 *    "early", the first early plugin and the first service's kiln_plugin_main, and never changes after that.
 *  - No member changes the selection, reports to the core or grants anything. Each loader still writes its own
 *    kiln/state/<loader>.json.
 *  - The core also writes the same record to kiln/state/launch.json for the launcher and kiln-manage. No code in the
 *    process reads that file: read this table.
 *
 * Inputs to the decision (in this order; the first that applies decides):
 *  1. A start where Kiln runs no plugin, or nothing at all: safe idle known at boot (the game image cannot be parsed,
 *     an unverified build under policy verified-only), play-once no-mods, an invalid profile, kiln.json "plugins":
 *     false. Backend NONE, a reason of the "idle" class.
 *     An error while the core reads its inputs (it never ends Kiln's boot) is idle class too: UNDECIDED.
 *  2. kiln.json "shroudforge": {"backend": "hosted" | "installed" | "kiln"} (game level, not a side section). Without
 *     it, the older {"useInstalled": true} means "installed"; with neither, "hosted" (the default). When both keys are
 *     there, "backend" wins. Any other "backend" value is refused (backend NONE, "refused" class).
 *  3. hosted / installed: KILN_SHROUDFORGE_RUNTIME=installed|bundled overrides the choice between those two, as it
 *     did before this table. It never selects or overrides "kiln".
 *  4. kiln: the capability kiln-sfi.interpreter must not be held, and the profile (profile.json or the play-once
 *     profile) must name a launch plan that kiln-manage wrote for this game:
 *       "sfLaunchPlan": {"path": "launch-plans/<plan id>.json", "sha256": "<the file's SHA-256>"}
 *     resolved below <game>/kiln/. The file must be a plain file (no link) of at most 8 MiB with exactly that
 *     SHA-256, launch plan format 1 for this executable and side, selecting "kiln", carrying no launch id (the core
 *     gives each start its own), and needing nothing of ShroudForge 1.8 while the kiln-sfi.sf18 veto stands.
 *     Anything else refuses kiln for this start: backend NONE, "refused" class, and no other backend runs instead.
 *
 * Reading the backend, for a loader:
 *  - HOSTED, INSTALLED or KILN: that backend has this start; every other ShroudForge backend stands aside.
 *  - NONE with a reason of the "refused" class (0x100..0x1ff): the player asked for something Kiln will not run this
 *    start. Every ShroudForge backend stands aside, and the notice (notice_copy) says why.
 *  - NONE with a reason of the "idle" class (0x200..0x2ff): the core did not decide. Each loader follows the rules it
 *    had before this table.
 *
 * kiln.json "native": {"policy": ...} as the core read it at the freeze (native_policy, after the first 192 bytes of
 * the record): the value Kiln's native mods follow this start (always, verified-only or never). Under backend KILN it
 * gates ShroudForge package DLLs too: never, or verified-only on a build that is not verified (build_verified, beside
 * it: the same answer KilnBuildInfo.verified gives from Kiln's plugin step on, here already for the early plugins),
 * loads no package DLL, and every package that would load one is withdrawn whole with every package that depends on
 * it. Hosted and installed ShroudForge do not read either.
 *
 * Threads: status and notice_copy work on any thread, never block and never allocate on the caller's behalf.
 */
#ifndef KILN_LAUNCH_H
#define KILN_LAUNCH_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_LAUNCH_SERVICE "kiln.launch"
#define KILN_LAUNCH_VERSION 1u

/* KilnLaunchStatus.backend */
enum {
    KILN_LAUNCH_BACKEND_NONE = 0,      /* no ShroudForge backend has this start (reason_code says which class) */
    KILN_LAUNCH_BACKEND_HOSTED = 1,    /* kiln-shroudforge with Kiln's bundled ShroudForge runtime */
    KILN_LAUNCH_BACKEND_INSTALLED = 2, /* kiln-shroudforge with the player's own ShroudForge (<game>/shroudforge) */
    KILN_LAUNCH_BACKEND_KILN = 3,      /* kiln-sfi: Kiln's own interpreter */
};

/* KilnLaunchStatus.reason_code. The text (reason, notice_copy) says the details. */
enum {
    /* a backend was selected */
    KILN_LAUNCH_REASON_HOSTED_DEFAULT = 0x001, /* no kiln.json setting: hosted, the default */
    KILN_LAUNCH_REASON_SETTING = 0x002,        /* kiln.json shroudforge.backend */
    KILN_LAUNCH_REASON_USE_INSTALLED = 0x003,  /* kiln.json shroudforge.useInstalled: true (the older key) */
    KILN_LAUNCH_REASON_ENV_RUNTIME = 0x004,    /* KILN_SHROUDFORGE_RUNTIME chose between hosted and installed */
    KILN_LAUNCH_REASON_PLAN = 0x005,           /* kiln, with the launch plan the profile names */
    /* refused: every ShroudForge backend stands aside this start */
    KILN_LAUNCH_REASON_SETTING_INVALID = 0x100,   /* shroudforge.backend is not hosted, installed or kiln */
    KILN_LAUNCH_REASON_INTERPRETER_HELD = 0x101,  /* the capability kiln-sfi.interpreter is held */
    KILN_LAUNCH_REASON_PLAN_MISSING = 0x102,      /* no plan named, or the named file is not there */
    KILN_LAUNCH_REASON_PLAN_INVALID = 0x103,      /* the plan or its reference cannot be read, changed, or is malformed */
    KILN_LAUNCH_REASON_PLAN_STAMPED = 0x104,      /* the plan carries a launch id: not a plan kiln-manage wrote */
    KILN_LAUNCH_REASON_PLAN_OTHER_GAME = 0x105,   /* the plan is for another executable or side */
    KILN_LAUNCH_REASON_SF18_VETO = 0x106,         /* an admitted package needs ShroudForge 1.8 (the kiln-sfi.sf18 veto) */
    KILN_LAUNCH_REASON_EML_R_LOADER = 0x107,     /* EML-R's game-folder loader may run the same mods */
    /* idle: the core did not decide; each loader follows its rules from before this table */
    KILN_LAUNCH_REASON_SAFE_IDLE = 0x200,         /* safe idle known at boot */
    KILN_LAUNCH_REASON_NO_MODS = 0x201,           /* play-once no-mods */
    KILN_LAUNCH_REASON_PLUGINS_OFF = 0x202,       /* kiln.json "plugins": false */
    KILN_LAUNCH_REASON_PROFILE_INVALID = 0x203,   /* the profile could not be read or validated: no mods load */
    KILN_LAUNCH_REASON_UNDECIDED = 0x204,         /* the core could not decide (an error; kiln.log says which) */
};
#define KILN_LAUNCH_REASON_REFUSED(code) ((code) >= 0x100u && (code) < 0x200u)
#define KILN_LAUNCH_REASON_IDLE(code) ((code) >= 0x200u && (code) < 0x300u)

#define KILN_LAUNCH_REASON_BYTES 128

/* KilnLaunchStatus.native_policy: kiln.json native.policy (the same value native mods follow this start). */
enum {
    KILN_LAUNCH_NATIVE_POLICY_ALWAYS = 0,        /* "always" (kiln.json's default) */
    KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY = 1, /* "verified-only": only on a verified build (KilnBuildInfo.verified) */
    KILN_LAUNCH_NATIVE_POLICY_NEVER = 2,         /* "never" */
};

/* The first record (size up to and including reason): what a core before native_policy fills, and the smallest size
 * any core accepts. */
#define KILN_LAUNCH_STATUS_V1_SIZE 192u

/* The frozen record, size-versioned. The caller sets size to sizeof(KilnLaunchStatus); a size below
 * KILN_LAUNCH_STATUS_V1_SIZE is refused. The core fills the largest record it knows that fits the caller's size (192
 * or 200 bytes) and sets size to it: a caller built before native_policy (192 bytes) gets the first record's fields
 * and size 192, and nothing past them is written; a later, larger struct can tell what an older core left out. A
 * reader that needs native_policy or build_verified checks size >= 200 first: a core that filled 192 did not report
 * them. */
typedef struct KilnLaunchStatus {
    uint32_t size;
    uint32_t backend;      /* KILN_LAUNCH_BACKEND_* */
    uint32_t frozen;       /* 1 whenever status returns KILN_OK */
    uint32_t reason_code;  /* KILN_LAUNCH_REASON_* */
    uint64_t launch_id;    /* this start's own id, fresh in every process, never 0 */
    uint8_t plan_hash[32]; /* SHA-256 of the launch plan read for this start (kiln, or a refused plan); else all 0 */
    uint64_t freeze_stamp; /* when the record froze: UTC milliseconds since 1970-01-01 */
    char reason[KILN_LAUNCH_REASON_BYTES]; /* why, UTF-8, NUL-terminated, cut at a character boundary when longer */
    /* appended (200 bytes): */
    uint32_t native_policy; /* KILN_LAUNCH_NATIVE_POLICY_*: kiln.json native.policy as the core read it at the freeze */
    uint32_t build_verified; /* 1: this game build is verified, the answer the boot computed before the freeze (the one
                                KilnBuildInfo.verified serves from Kiln's plugin step on); 0: not verified, and always 0
                                for backend NONE */
} KilnLaunchStatus;

typedef struct KilnLaunchApi {
    KilnServiceHeader header; /* { sizeof(KilnLaunchApi), KILN_LAUNCH_VERSION } */
    /* The frozen record. KILN_ERR_INVALID_ARGUMENT: out is NULL or out->size < KILN_LAUNCH_STATUS_V1_SIZE.
     * KILN_ERR_UNAVAILABLE: not frozen yet (no plugin ever sees that; the core freezes before any of them runs):
     * out->frozen is 0 and nothing else is filled. */
    KilnResult(KILN_CALL* status)(KilnLaunchStatus* out);
    /* The notice for the player about this start's selection, for a loader that has to show it itself when Kiln's own
     * UI is not there: empty for hosted, installed and the idle class; one or two sentences for kiln and for a
     * refusal. UTF-8, not NUL-terminated. *out_len (when out_len is not NULL) gets its length in bytes; with a NULL
     * buffer or a capacity below that length, KILN_ERR_BUFFER_TOO_SMALL and nothing is copied. KILN_ERR_UNAVAILABLE:
     * not frozen yet. The text never changes after the freeze. */
    KilnResult(KILN_CALL* notice_copy)(char* buffer, size_t capacity, size_t* out_len);
} KilnLaunchApi;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define KILN_LAUNCH_ASSERT static_assert
#else
#define KILN_LAUNCH_ASSERT _Static_assert
#endif
KILN_LAUNCH_ASSERT(sizeof(KilnLaunchStatus) == 200 && offsetof(KilnLaunchStatus, backend) == 4 &&
                       offsetof(KilnLaunchStatus, frozen) == 8 && offsetof(KilnLaunchStatus, reason_code) == 12 &&
                       offsetof(KilnLaunchStatus, launch_id) == 16 && offsetof(KilnLaunchStatus, plan_hash) == 24 &&
                       offsetof(KilnLaunchStatus, freeze_stamp) == 56 && offsetof(KilnLaunchStatus, reason) == 64 &&
                       offsetof(KilnLaunchStatus, native_policy) == KILN_LAUNCH_STATUS_V1_SIZE &&
                       offsetof(KilnLaunchStatus, build_verified) == 196,
                   "KilnLaunchStatus layout (the first record 192 bytes, native_policy and build_verified appended: 200)");
KILN_LAUNCH_ASSERT(sizeof(KilnLaunchApi) == 24 && offsetof(KilnLaunchApi, status) == 8 &&
                       offsetof(KilnLaunchApi, notice_copy) == 16,
                   "KilnLaunchApi v1 layout (24 bytes, x64)");
#undef KILN_LAUNCH_ASSERT

#endif /* KILN_LAUNCH_H */
