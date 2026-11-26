# Registry conflicts

The launcher's **Conflicts** page reads the selected game's `kiln/state/registry.json`. This is a saved snapshot, not
live telemetry, and it does not change mods, profiles, load order or registry values. Profiles remain in Settings.

For each retained field, expand the row to see the base origin and value, the owners and operations in application
order, each intermediate value, and the recorded winner and result. Earlier omitted writers are counted explicitly.
Refused edits retain their reasons, including `RestartRequired` and `Unproven`. Conflict attribution distinguishes
**Certain**, **Named from journal**, **Best-effort drift**, and **Unknown**; best-effort detection does not prove who
wrote a value or that no other writer exists.

**Waits for a restart** (since 0.2.0-dev.31.14) comes first when the snapshot has refused rows whose reason starts with
the stable token `RestartRequired: `: the edits a reload, a runtime registry pass or a live edit left for the next start
(a restart field a reloaded pack changed, a field the start never read, a section or entry operation added after the
start). It shows `counts.restartRequired` (every such row, also those the native cap left out) and each retained row's
mod, place and reason; the same rows stay in Refused edits.

**Registry plan** (since 0.2.0-dev.31.14) names the plan the last start ran on, the previous (last good) plan, the
rollback of the last start (applied or refused, with why) and a rollback request waiting for the next start.
**Roll back** first shows the dry run: the plan it goes to, its mods and versions, and every value that changes back
(`field: from → to`, at most 50 shown); nothing is written until **Confirm roll back**. **Clear** (when a request waits)
says the next start runs on the mods' current edits again and removes the request on **Confirm clear**. Saves are never
touched; the request takes effect at the next start, which refuses the plan whole when one of its mods is not loaded at
its version, one of its edits is refused on that game build, or it would change what saves hold
([registries README](../modding/registries/README.md#plans-and-rollback)). The same as `kilnctl registry rollback`.

A missing file is different from a valid snapshot with no edits. Unreadable, malformed, unsupported and oversized
reports show an error with **Refresh report** available. A stopped game, a report older than the current process start,
an old snapshot (over 24 hours), or an unverifiable process start/clock is marked **Stale snapshot**. Its saved values
remain readable. Windows report PIDs are not compared with host PIDs because Proton uses a different PID namespace.
The page refreshes the snapshot periodically and also offers an explicit refresh.

## Launcher commands

`registry_rollback({ gameId: string, clear: boolean, dryRun: boolean })` (since 0.2.0-dev.31.14) works on the known game
folder's `kiln/data/content/registry/` through `kiln_manage::registry_plan`, as `kilnctl registry rollback`: with `dryRun`
it writes nothing and answers `{plan, previous, rollback, pending, target: {hash, mods}, changes: [{field, from, to}],
changesUnknown, blocked, dryRun, wrote, cleared}`; without it, it writes the request for the previous (last good) plan
(`by: "launcher"`), or with `clear` removes the request. `blocked` says why a rollback cannot be asked for (no plan yet, no
earlier good plan, the previous plan's file gone or written by an older Kiln); `changesUnknown`, why the changes could not
be computed (the start lists them under `rollback.changedBack`).

`registry_report_read({ gameId: string })` returns a UTF-8 string, or `null` when no file exists, using the launcher's
normal structured error on I/O failure. The caller cannot supply a path. The native reader resolves the known game
folder, refuses non-regular files and paths outside it, and caps both the metadata size and actual read at **8 MiB**.
The frontend separately caps bytes before parsing, JSON nesting at 48, and containers (objects and lists) at one per 16 bytes of the byte cap (524,288); separators are not counted, so any real report up to 8 MiB parses.
Unknown format-1 members are ignored; unsupported formats and malformed consumed rows are errors. The native writer
caps rows, not bytes, so a very large mod set (towards 20,000 fields with long writer chains) can write a report over
8 MiB: the page then says so and points to `kiln/state/registry.json` in the game folder, which holds it in full.

The view retains at most 500 rows per report section, 64 final writers per field, 2,048 characters per text value,
and bounded list/record previews (16 members, depth 4, 128 visited values). Both native omission counts and launcher
omissions are disclosed. Sections paginate at 25 rows; the field filter searches only retained rows. Formula inputs
are shown in evaluation order. This is intentionally not an export of the full history.

All report text is rendered as React text, never HTML. Mapping records, hashes, raw member paths, game addresses and
function metadata are not presented. Rows the runtime marks `raw` (Advanced-tier raw paths: field rows, conflict lines
and refused edits) keep their outcome but hide the path, and conflict or refusal text on a Kiln field that mentions a
raw path a mod wrote shows "raw path (details hidden)" in its place. Other field names are checked against Kiln's published
registry identifier lock: a name it does not know (a misspelled field, or a field of a newer Kiln) is shown as written,
marked "Field unknown to this launcher", and a refusal keeps its native reason, which is what the mod author can act on. No game client is launched by this page.
