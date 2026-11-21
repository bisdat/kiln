# kiln.launch / The ShroudForge backend of this start

**Kiln-internal:** the core and Kiln's own loaders read this table (kiln-sfi, kiln-shroudforge, kiln-eml); it is not in
the SDK's headers and is no mod API.

One record per process: which ShroudForge backend runs this start (Kiln's bundled ShroudForge, the player's installed
one, Kiln's own interpreter, or none), why, which launch plan it was decided from, kiln.json `native.policy` and whether
the game build is verified. The core decides it once at boot
and freezes it before any native mod of phase "early", any early plugin and any service's `kiln_plugin_main` runs.
Nothing changes it afterwards: no member reports to the core or grants anything. Loaders read it instead of deciding
for themselves, so two ShroudForge backends never both start.

**Status:** host tests and headless Wine tests (no game, no lab). kiln-sfi, kiln-shroudforge and kiln-eml follow it
([What the loaders do](#what-the-loaders-do)); kiln.engine reads the dialect owner (below).

| Layer | Where |
|---|---|
| C table | `native/core/abi/services/kiln_launch.h` (`kiln.launch` v1, in-core: always there, in every view and in safe idle) |
| Native | `native/core/src/launch.cpp` (the validator, the freeze, the tables, the dialect owner, the state file's text; portable), `launch_boot.cpp` (what the boot reads: kiln.json, the environment, the profile, the client gate, the plan file; the state file's write) |
| Release hold | `native/core/gates/sfi.json` (the held rows), `native/core/src/sfi_release_policy.{h,cpp}` (the `kiln-sfi.sf18` veto, a row held on one side only); [below](#the-held-row-and-the-kiln-sfisf18-veto) |
| Internal | `native/core/src/launch_internal.h` (`kiln.internal.launch` v1: the ShroudForge-dialect owner, for Kiln's own services); `api.cpp` `load_one` designates it |
| File | `kiln/state/launch.json` (the same record for the launcher and kiln-manage; nothing in the process reads it) |
| Boot | `native/core/src/boot.cpp`: frozen after compat's early rules, before the native mods are discovered; in the safe idle of an image that cannot be parsed, before the chain's `[late]` DLLs |
| Rust | `crates/sfi/kiln-abi` (`services::kiln_launch`, generated); `crates/sfi/kiln-sfi/src/lib.rs` `read_launch`, `read_build_verified`, `Boundary::native_permission`, `Boundary::native_run`; `crates/sfi/kiln-sfi/src/native_manager/arming.rs` (when the native loader is armed); `crates/sfi/sfi-policy/src/native_selection.rs` `NativePermission`; `gpl/kiln-eml/kiln-eml/src/plugin.rs` `read_launch` and `kiln_mode.rs` (kiln-eml under `kiln`) |
| kiln-manage | `crates/kiln-manage/src/sfi_backend.rs`: the kiln.json setting read and written as the core decides it (the launcher's side; no idle class); `sfi_plan.rs` (feature `sfi-scan`): the launch-plan producer; `sfi_inspection.rs`: kiln-eml's read-only archive inspector as the producer runs it |
| Tests | `native/core/tests/test-launch.sh` (the decision table, the record and a 192-byte reader of it, readers and designations racing under TSan, the boot order cut out of boot.cpp with `build_verified` as the early plugins read it, the designation in `load_one`, native.policy and `build_verified` in `launch_boot.cpp`, the state file), `test-service-access.sh` (every view resolves `kiln.launch`, only the services' view `kiln.internal.launch`; nobody publishes either), `test-launch-wine.sh` (launch.json replaced atomically on the Windows code path under Wine, once per boot), `test-abi-headers.sh` (its layout as C and C++) |

## Reading it
```c
const KilnLaunchApi* launch = api->service((KilnStr){"kiln.launch", 11}, 1);
KilnLaunchStatus st = {sizeof st};  /* 200 bytes */
if (launch && launch->status(&st) == KILN_OK) {
    if (st.backend == KILN_LAUNCH_BACKEND_KILN) { /* kiln-sfi has this start */ }
    else if (KILN_LAUNCH_REASON_REFUSED(st.reason_code)) { /* every ShroudForge backend stands aside; show the notice */ }
    else if (st.backend == KILN_LAUNCH_BACKEND_NONE) { /* the core did not decide: the loader's own rules */ }
}
```
- `status` and `notice_copy` work on any thread and never block. The record never changes after the freeze.
- A null table is an older core: it decides nothing, and every loader keeps its rules from before this table.
- `status` before the freeze answers `KILN_ERR_UNAVAILABLE` with `frozen` 0. No plugin, native mod or service ever
  sees that: the core freezes before any of them runs.
- `reason` is cut at 127 bytes on a character boundary; `notice_copy` has the whole text.
- The record is size-versioned. The first record is 192 bytes (up to `reason`); `native_policy` and `build_verified`
  follow (200). The core fills the largest record that fits the caller's `size` and sets `size` to it: a reader built
  before `native_policy` (192 bytes) gets every field it knows, `size` 192, and nothing past its 192 bytes is written. A
  size below 192 (`KILN_LAUNCH_STATUS_V1_SIZE`) is refused. A reader that needs `native_policy` or `build_verified`
  checks `size` >= 200: a core that filled 192 never reported them.

## How the backend is chosen
The first rule that applies decides:

| Rule | Backend | Reason code |
|---|---|---|
| A safe idle known at boot (the image cannot be parsed, an unverified build under policy verified-only), an invalid profile, play-once no-mods, kiln.json `"plugins": false` | none | idle class: `SAFE_IDLE`, `PROFILE_INVALID`, `NO_MODS`, `PLUGINS_OFF` |
| kiln.json `"shroudforge": {"backend": ...}` that is not `"hosted"`, `"installed"` or `"kiln"` | none | `SETTING_INVALID` |
| `"backend": "hosted"` or `"installed"`; without it the older `"useInstalled": true` (installed); with neither, hosted | hosted / installed | `SETTING`, `USE_INSTALLED`, `HOSTED_DEFAULT` |
| ... and `KILN_SHROUDFORGE_RUNTIME` is set (1 to 15 characters): `installed` is installed, any other value hosted, as kiln-shroudforge reads it | hosted / installed | `ENV_RUNTIME` |
| `"backend": "kiln"` while the capability `kiln-sfi.interpreter` is held | none | `INTERPRETER_HELD` |
| `"backend": "kiln"` and the profile names no plan, or the named file is not there | none | `PLAN_MISSING` |
| ... a plan that cannot be read, changed since kiln-manage wrote it, or is not a valid format-1 plan selecting kiln | none | `PLAN_INVALID` |
| ... a plan that carries a launch id | none | `PLAN_STAMPED` |
| ... a plan for another executable or side | none | `PLAN_OTHER_GAME` |
| ... a plan that admits a package needing ShroudForge 1.8 | none | `SF18_VETO` |
| `"backend": "kiln"` with a valid plan | kiln | `PLAN` |
| An error while the core reads its inputs or decides (it never ends Kiln's boot; kiln.log says which) | none | idle class: `UNDECIDED` |

- The setting is read at game level (kiln.json's top-level `"shroudforge"`, not a side section). When both keys are
  there, `"backend"` wins. kiln.json that does not parse counts as no setting, as before.
- `KILN_SHROUDFORGE_RUNTIME` never selects or overrides `kiln`, and never rescues a refused setting.
- Nothing in the decision ends Kiln's boot: a JSON value in a reason is quoted short (a string cut to 64 bytes, an
  array or object by its kind), text that is not UTF-8 becomes `?`, launch.json's text never throws, and an exception
  while reading the inputs freezes `UNDECIDED` instead.
- **Three classes.** A backend: that backend has this start. None with a *refused* reason (0x100-0x1ff): the player
  asked for something Kiln will not run; every ShroudForge backend stands aside and the notice says why, with no other
  backend in its place. None with an *idle* reason (0x200-0x2ff): the core did not decide, and each loader follows its
  rules from before this table.
- A safe idle decided after the freeze (symbol definitions that do not resolve) does not change the record. No
  plugin's `kiln_plugin_main` runs in that start.

## kiln.json native.policy in kiln mode (LP1a, LP1b)
`native_policy` is kiln.json `"native": {"policy": ...}` as the core read it at the freeze, side section included: the
value Kiln's native mods follow the same start (`KILN_LAUNCH_NATIVE_POLICY_ALWAYS` 0, `VERIFIED_ONLY` 1, `NEVER` 2).
It is not part of the backend decision and is filled for every record, `UNDECIDED` included. (The safe idle of a game
image that cannot be parsed freezes before any side section applies, so its record has the top-level value.)

`build_verified` (LP1b) is 1 when this game build is verified, else 0: the boot's own `build_verified` answer, computed
before the freeze (`boot.cpp` hands it to `freeze_at_boot`), the same value `KilnApi.build_info().verified` serves from
Kiln's plugin step on (`set_runtime_state`; the early plugins get 0 there). It is not part of the decision either, and it
is 0 for backend none whatever the boot answered (the safe idle of an image that cannot be parsed never asks).

Under backend `kiln` the policy gates ShroudForge package DLLs exactly as it gates native mods (`native_mods.cpp`):
- **Refused** when the policy is `never`, or `verified-only` while this game build is not verified.
  kiln-sfi's early phase takes `verified` from the record's `build_verified` (never from `build_info` there); a value
  other than 0 or 1 counts as not verified. Its plugin step reads `build_info().verified` again: when that differs from
  the record, the interpreter is refused this start (sfi.json `refused`, code `verified-disagrees`, one loud warning),
  whatever the policy.
- **The native manager**: right after the plan read, in the same phase, kiln-sfi makes the one native run of the
  process from the plan and the record. Its loader (the loader notification, its supervisor thread, then the ordered
  loads on one plain thread, `kiln-sfi native`) is armed only when the run was made in the early phase, the policy
  allows, the plan lists at least one DLL and the startup witness is wired; a run kiln-sfi had to make at its plugin
  step (its early phase never ran) is concluded at once, because package DLLs load only in the early window. The
  startup witness is wired when kiln-eml's startup seam is attached this start (Kiln's startup pass,
  [eml-startup.md](eml-startup.md)). A run whose loader is not armed refuses every planned DLL at once with the reason
  (the policy's words first, else the missing witness): no notification is registered, no thread starts and nothing is
  captured, hashed or mapped. A plan with no DLL arms nothing either. `kiln_plugin_early` never
  waits for the loader.
- **When refused**, no package DLL loads, not one is even mapped. Every package with a selected, profile, ini or
  declared `register_dll` DLL is withdrawn whole (and, while the loader is not armed, every package with the
  `runtime-register-dll` capability), and so is every package that depends on one, through Lua-only packages too.
  sfi.json gives each planned DLL its row (`refused`) and each such package its skip (`native-refused` with the reason). The reason is Kiln's own text: `kiln.json native.policy is "never"`, or
  `kiln.json native.policy is "verified-only" and this game build is not verified`. Each refused DLL's own status says
  the same (a DLL with an issue of its own keeps that issue); a runtime `register_dll` call gets it as its Lua error.
  kiln.log has one warning from kiln-sfi. Packages that need no DLL run, so the interpreter still starts.
- **When allowed** (`always`, or `verified-only` on a verified build), nothing changes.
- **Boot wins.** kiln-manage's plan producer (`crates/kiln-manage/src/sfi_plan.rs`) applies `never` the same way
  (`sfi_policy::native_selection::NativePermission`) when it writes the plan; `verified-only` it leaves to the boot,
  since only the core knows whether the build is verified. Whatever the plan says, the boot decision withdraws at start
  and never loads.
- **Hosted and installed ShroudForge never read it.** Their first record (the first 192 bytes) is byte for byte the same
  under every policy; only `native_policy` and `build_verified` themselves and launch.json's `nativePolicy` name it.
  launch.json does not carry `build_verified`.
- **A record without it** cannot start the interpreter. kiln-sfi asks for 200 bytes, so a core that fills only 192, or
  a `kiln` record with a policy value it does not know, reads as no table: the rule from before the table, so nothing
  starts.

Tests:
- `native/core/tests/test-launch.sh`: the record under each policy, backend and `build_verified` (0 for backend none), a
  192- and a 199-byte reader, the race, launch.json, `launch_boot.cpp`'s mapping from `config()` and its
  `build_verified` from boot's `verified`, and the early plugins reading `build_verified` in the boot order under each
  verified x policy combination.
- `crates/sfi/sfi-policy/tests/native_selection.rs`: the selection, every DLL source, Lua-only dependents and the sealed
  `register_dll` table.
- `crates/sfi/kiln-sfi/tests/native_manager.rs`: the executor, with zero loads; the arming rule over every combination of
  its three terms with a counting fake backend (no registration, fault source or thread unless all three hold, the
  registration before the first load when they do); a run concluded without the loader.
- `crates/sfi/kiln-sfi/tests/native_wiring.rs`: the early phase's one run from the record (never `build_info`), the
  arming rule from the record, the plugin step's `verified-disagrees` refusal, the terminal in admission and sfi.json.
- `crates/sfi/kiln-sfi/tests/launch.rs`: the record read, `build_info` at the plugin step, the warning, and
  hosted/installed untouched.
- `crates/sfi/kiln-sfi/tests/wine_bindings.rs`: `manager-order-never`, `manager-witness-change-never` and
  `manager-order-unverified`; `boundary-never`, `boundary-unverified` and `boundary-not-wired` (kiln-sfi's early phase
  with the production loader: no loader notification, no extra thread, nothing mapped) and their control
  `boundary-armed`; under Wine and natively on Windows.

## What the loaders do
| Record | kiln-shroudforge | kiln-sfi | kiln-eml |
|---|---|---|---|
| hosted / installed | runs that runtime. Its fallback to the other runtime when the chosen one is missing or broken stays, and the profile still says whether ShroudForge mods are on (`KILN_SHROUDFORGE=0`: off, reason `KILN_SHROUDFORGE=0`) | stands aside | told to stand aside by kiln-shroudforge when it hosts, as before |
| kiln | stands aside before anything else: `shroudforge.json` mode `off`, the reason names the interpreter; `KILN_SHROUDFORGE=1` does not start a runtime | reads kiln.json `native.policy` and `build_verified` from the record, makes its native run in the early phase and checks `build_info().verified` against the record at its plugin step (above); reads the frozen launch plan once (see [The launch plan](#the-launch-plan)) and starts the interpreter at the plugin step's `on_ready`: its worker follows that read; a plan it cannot follow (changed since the freeze, or refused by its strict reader) is `refused` with a loud warning, never hosted, and a kiln-sfi without a worker refuses too. ShroudForge's own loader is looked for again right before the start; if it arrived after main, kiln-sfi stands aside with the same reason | the plan's data writer: it refuses `stand_aside` (`KILN_ERR_UNAVAILABLE`, one kiln.log warning) and never reads `shroudforge.json`. It reads the plan again (see below) and decides each package by it: admitted in ShroudForge's dialect with startup data, run by Kiln's startup pass ([eml-startup.md](eml-startup.md)); admitted without startup data, or with startup dialect `none`, left to the interpreter; listed but not admitted (`skip`, `disabled`, `other-side`), never run, whatever it needs; needing ShroudForge's runtime and not admitted, skipped with a reason that names the plan; admitted for the `eml` dialect, or not listed at all, run as an EML mod as before. A plan it cannot read again runs no EML mod. `KILN_SHROUDFORGE=0`: its rules from before |
| none, refused class | stands aside: mode `off`, the reason quotes the record | stands aside | never told to stand aside; its rules from before |
| none, idle class; no table; a table or record it cannot read | its rules from before this table: it reads kiln.json itself, `backend` > `useInstalled` > hosted (byte for byte as before when there is no `backend`; `"kiln"` or a value that is none of the three: it stands aside, and its log line and status name the value as the core does, a container by its kind and a long string cut) | does nothing | as before |

- ShroudForge's own loader in the process (its bootstrap or its runtime): kiln-shroudforge reports `self-hosted` as
  before, and kiln-sfi stands aside under `kiln` too, also when the loader arrives between kiln-sfi's main and its
  `on_ready`. The record decides, not the early plugins' file order.
- `KILN_SHROUDFORGE=0` stands every ShroudForge backend aside, Kiln's interpreter included, with one reason,
  `KILN_SHROUDFORGE=0` (kiln-shroudforge's status and log line, kiln-sfi's capability row). For hosted and installed
  that is kiln-shroudforge's off note as it always was.
- With a record that decided, kiln-shroudforge never reads kiln.json for the backend: the core reads it as JSONC
  (comments and trailing commas allowed), kiln-shroudforge's own reader does not, and the two must not disagree.
- `shroudforge.json` gets `"launch": {"backend", "reasonCode", "reasonClass", "reason", "launchId"}` (the launch id as
  16 lower-case hex digits) only when the record decided (a backend, or refused); kiln-sfi says the same in its
  capability row and kiln.log.
- kiln-eml reads the plan the record froze again, from the profile's `sfLaunchPlan` path, and uses it only when its
  SHA-256 is the record's `plan_hash`. As the core does, it wants a plain `kiln/launch-plans` folder and a plain file
  opened without following a link and checked through that handle; it hashes and parses the same bytes. Of the members
  it uses, it refuses what kiln-sfi's validator refuses (an id twice or empty, a root that is not `mods/<one folder>`,
  an unknown decision, a `skip` without a reason). Every record but `kiln`, and `kiln` under `KILN_SHROUDFORGE=0`,
  leaves its start as without the table, byte for byte (eml.json, kiln.log, eml.log and the install's files).
- Tests: `native/plugins/shroudforge/tests/launch.py` (the adapter and kiln-sfi.dll in one fake host with a fake
  `kiln.launch`; Wine, and natively on Windows), `crates/sfi/kiln-sfi/tests/launch.rs`,
  `crates/sfi/kiln-sfi/tests/launch_worker.rs` (the worker under the record), the setting in
  `crates/kiln-manage/src/sfi_backend.rs`, and kiln-eml's `kiln_mode_tests.rs`, `plugin_records_tests.rs` (every
  record against no table, through the plugin) and `plugin.rs` tests (`cargo test -p kiln-eml --lib`).

## The launch plan
kiln-manage writes one immutable plan per activation under `<game>/kiln/launch-plans/<plan id>.json` and names it in
the profile (`profile.json`, or the play-once profile):
```json
"sfLaunchPlan": {"path": "launch-plans/0f0e0d0c-0b0a-4908-8706-050403020100.json", "sha256": "<the file's SHA-256>"}
```
The core reads it only when kiln is selected and checks what the selection depends on; kiln-sfi reads the whole plan
again and refuses package by package (`crates/sfi/sfi-policy/src/launch_plan.rs`):
- the path is exactly `launch-plans/<plan id>.json` with a canonical lower-case plan id: no other file can be named;
- a plain file (no folder, no link), at most 8 MiB, whose SHA-256 is the one the profile names;
- strict JSON: a key twice in one object or nesting deeper than 64 levels is refused;
- format 1 and minReader 1, the plan id of its file name, `launch_id` null (each start gets its own id from the core),
  written by `kiln-manage`, for this executable's SHA-256 and side, requested and selected `kiln` with `kiln-eml` as
  its data writer;
- no admitted package (`decision: "candidate"`) needs ShroudForge 1.8 (`requires180`, or a DLL on bridge ABI 2), unless
  the release policy allows 1.8 and the plan says a final 1.8 exists. In this build the release policy never does
  (the `kiln-sfi.sf18` veto);
- `game.archiveBaseOrigin`, when present, is one of `first-managed-unclassified`, `rebaselined-unclassified` and
  `unrecorded` (kiln-eml's read-only inspector's token for the archive base, never "vanilla"); a plan without it is
  still read.

**The producer** (`crates/kiln-manage/src/sfi_plan.rs`) writes a plan only when kiln.json selects `kiln` and the
interpreter's hold is lifted, from the pre-select inventory of `<game>/mods`, Kiln's own compatibility rules
(`<game>/kiln/config/sfi/compatibility/rules.json`) and the archive inspector's answer; when anything it needs cannot be
read or proven it writes none, so the boot refuses `kiln` (`PLAN_MISSING`) with every ShroudForge backend aside
(also when `<game>/mods` is a link or junction, or cannot be listed in full; a package folder it cannot open is named
in a warning and not planned). The
file is created under a temporary name and given its plan id's name only while no file has it; equal inputs and an equal
id give equal bytes. Hosted and installed starts get no plan.

The record carries the plan's SHA-256 whenever one was read, also for a refused plan.

**kiln-sfi's read** (`crates/sfi/kiln-sfi/src/plan_source.rs`): once per process, in its early phase (at main only when the
early phase never ran), and only when it has the start and a worker. The profile text comes from kiln.profile (never
`profile.json` from disk, never `kiln/state/launch.json`); the folder and the file are opened as the core opens them (a
plain folder, no link or other reparse point, the opened handle checked to be the checked file, shared for reading
only, at most 8 MiB); the bytes read must hash to the record's `plan_hash` (else `refused`, `plan-changed`), and the
same bytes go through the strict reader against `build_info`'s executable hash and side (else `refused`,
`plan-invalid`). Its worker gets that read, never the file. What runs:
- only candidates; `skip`, `disabled` and `other-side` packages never register;
- a package that ships native code (a planned DLL, or `runtime-register-dll`) runs only as Kiln's native manager
  concluded (see [above](#kilnjson-nativepolicy-in-kiln-mode-lp1a-lp1b)); until the startup witness is wired the loader
  is never armed, so every such package is withdrawn with that reason; a package with startup data is held while no
  startup pass commits it (kiln-eml skips that data under the same plan); every package that depends on one of them is
  withdrawn too, through Lua-only packages;
- the exception: an EML package without a Lua part in a plan of startup dialect `eml` is kiln-eml's (its EML pass
  applies the data this start), so kiln-sfi withdraws nothing of it; a package that depends on it waits
  (`startup-unwitnessed`) until a startup outcome that names it reaches kiln-sfi;
- at runtime-create each remaining package folder must still have the plan's `content_sha256` (`sfi_policy::digest`:
  every file's relative path, size and SHA-256, sorted and length-framed) and be found by ShroudForge's discovery of
  `<game>/mods` (no ShroudForge profile catalog) under its planned id; otherwise it is withdrawn with its dependents.

Each withdrawal is one kiln.log warning and a skip in `kiln/state/sfi.json`. A package the plan itself skips gets the
plan's reasons as its skip (`plan-skip`) and, for a ShroudForge or native-selection package, the same warning (kiln-eml
says it for its own kind). Nothing below `<game>/shroudforge` is read.

## The ShroudForge-dialect owner (kiln.internal.launch)
Kiln's own services (kiln.engine's write gate) need to know which owner speaks ShroudForge's dialect, so that a
Kiln-made "busy" never latches it for the rest of a server step. `kiln.internal.launch` v1 has one member,
`dialect_owner()`: that owner, or 0.
- It resolves only through the services' `KilnApi` ([Kiln-internal tables](README.md#kiln-internal-tables)); plugins,
  the managed host and every other caller get nothing, and nobody can publish the name, not even a Kiln service.
- The core designates at most one owner per process: the owner it passes to `kiln_plugin_main` of
  `<game>/kiln/plugins/kiln-sfi.dll`, and only when kiln.launch froze backend `kiln` and that DLL's
  `kiln_build_stamp()` is this core's build identity (the test that gives a service DLL Kiln-internal tables). A DLL
  of that name elsewhere (a profile entry), another name, or a DLL of another build is never designated.
- It is set before that `kiln_plugin_main` runs, so before the first tick, and never changes or moves to another
  owner, also when that plugin's main fails. No member sets it and no flag opts out of it.
- kiln.log says which owner was designated, or why kiln-sfi.dll was not.

### kiln.engine's write gate admits it without the busy latch
The engine's write gate (`kiln.internal.engine`'s `ecs_gate_acquire`) has two admissions: `KilnMod` latches an owner
for the rest of the server step after any busy refusal (`kiln-gate-busy-this-step`), and `RetryKilnHolder` never
latches and retries a refusal caused only by another Kiln holder. The designated owner always gets the second one:
- kiln.engine looks `kiln.internal.launch` up once, in its `kiln_plugin_main`, and keeps `dialect_owner`. Services
  load before plugins, so nobody is designated yet then: `gate_acquire` asks `dialect_owner()` at each acquire (one
  atomic load in the core, before any engine word).
- When the acquiring owner is the designated one, the admission is `RetryKilnHolder`, whatever `EcsGate.admission` says.
  The record is left as the caller set it. No caller chooses this: kiln.world's compare-exchange and every other member
  keep passing `KilnMod`, and the decision is made only here.
- So the designated owner is never latched. A refusal caused only by another Kiln holder is retried up to 128 times
  with no pause, before any engine word. An engine word (`ecs-thread-registry-busy`, `ecs-scene-frame-busy`) is
  refused at once, as for everyone, and its next attempt in the same step tries the engine again. ShroudForge has one
  mods thread whose CAS only ever races the engine, so this is not a difference from it.
- The retries take well under a microsecond (about 0.3 µs measured on x86-64), far less than a hold, so they only
  catch a holder that is just leaving. When another Kiln holder's hold outlasts them, the designated owner still gets
  `kiln-gate-busy` (not latched), about as often as Kiln mods hold the gate.
- Every other owner keeps the latch exactly as before. With no table (an older core, or a kiln-engine.dll without
  Kiln-internal access), a table of another version, one too short or one without the member, or nobody designated,
  the gate is exactly as without this rule. Nothing is logged either way.
- Tests: `native/services/engine/tests/gate_test.cpp` covers the gate itself: the retry count, engine words, the live
  step, the record, no accessor, and a race under TSan with the designation made from another thread.
  `plugin_host_test.cpp` covers the real `kiln_plugin_main`: one lookup at init, the designation made after it, and
  absent, older, newer, short and empty tables. `world/tests/ecs_writes_test.cpp` covers kiln.world's compare-exchange
  right after a gate-busy one in the same step, and inside another owner's hold. `windows_host.cpp` runs the
  cross-built gate under Wine and on Windows.

## kiln/state/launch.json
The same record for the launcher and kiln-manage, written once per boot by the freeze (also in safe idle). No code in
the process reads it: plugins read the table. Format 1:
```json
{"format": 1, "kiln": "0.2.0-dev+20261119.abc1234", "pid": 4242, "launchId": "0123456789abcdef", "frozen": true,
 "backend": "kiln", "reasonCode": 5, "reasonClass": "selected", "reason": "...", "notice": "...",
 "planSha256": "<64 hex digits>", "planId": "0f0e0d0c-0b0a-4908-8706-050403020100", "freezeStamp": 1763500000123,
 "nativePolicy": "always"}
```
- `launchId` is hex: a 64-bit id does not fit a JSON reader's double. `reasonClass` is `selected`, `refused` or `idle`.
  `reason` is the whole text (the table cuts it to 127 bytes). `planSha256` is null when no plan was read, `planId`
  null unless the plan was accepted. `nativePolicy` is the record's `native_policy` in kiln.json's words (`always`,
  `verified-only` or `never`).
- Written after the record is served, atomically (`<file>.tmp`, then a replacing rename): a reader never sees half a
  record. On Windows that rename is refused while another handle on the old file is open, a launcher's read included,
  so the write is retried 20 times, 25 ms apart; if all fail, kiln.log says so and the file keeps an earlier start's
  record (its `pid` and `launchId` tell).

## The held row and the kiln-sfi.sf18 veto
Both live in the client gate (`native/core/gates/README.md`):
- `native/core/gates/sfi.json` holds `kiln-sfi.sf18` (`sfi-sf18-final`) on both sides, and `kiln-sfi.interpreter` on
  both sides only while Kiln's interpreter is a preview (checks `sfi-backend-roundtrip`, `sfi-save-sensitive`,
  `sfi-three-surface-warning`, `sfi-ui-actions-notices`, `sfi-ui-page`, `sfi-ui-settings`,
  `sfi-warning-accessibility`). kiln.json `"preview"` lifts them as any row: `true`, or a list naming them. Without the
  `kiln-sfi.interpreter` row Kiln's interpreter is a normal choice (Settings > ShroudForge, kiln.json
  `shroudforge.backend: "kiln"`), no `"preview"` needed; hosted ShroudForge stays the default.
- `native/core/src/sfi_release_policy.{h,cpp}`, the release policy, holds on top of the list, and `"preview"` never lifts:
  - `kiln-sfi.sf18` while the build records no final ShroudForge 1.8.x (`kUpstream180Final`, false in every release and
    preview build). Only a reviewed change of that constant, in a new Kiln build, opens it; no kiln.json key, profile,
    launch plan (`upstream180Final`) or environment variable is read for it. It holds whatever the list holds;
  - `kiln-sfi.interpreter` when the list holds it on one side only: held on both sides instead of opened on the other.
    A list with no row of that name holds nothing here. A malformed list stops the build already (`gates/gen.py`).
- The policy's rows read `unavailable`, reason `policy`, with a text that begins with `game test pending: ` like every
  held row, so `kiln_caps_held()` is true for them and `KilnApi.feature()` false. kiln.log names them at boot ("stays
  held by the release policy").
- At boot, `launch_boot.cpp` takes the interpreter's hold from the client gate alone (after `"preview"`, policy
  included) and the veto from `sfi_release_policy::release_allows_180()` alone. `test-launch.sh` checks both in the
  source.
- kiln-manage reads the same list (compiled in) and the same record (`crates/kiln-manage/src/sfi_release.rs`) for the
  launcher's warnings; the core's decision at boot is the one that counts.
- Tests: `native/core/tests/test-sfi-release-policy.sh` (the policy through the gate, ASan+UBSan and TSan), the `sfi:`
  rows of `caps_test.cpp`, `tools/sfi/test_public_cut.py`, and kiln-manage's `sfi_release` tests.
