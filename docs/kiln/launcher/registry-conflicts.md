# Registry conflicts

The launcher's **Conflicts** page reads the selected game's `kiln/state/registry.json`. This is a saved snapshot, not
live telemetry, and it does not change mods, profiles, load order or registry values. Profiles remain in Settings.

For each retained field, expand the row to see the base origin and value, the owners and operations in application
order, each intermediate value, and the recorded winner and result. Earlier omitted writers are counted explicitly.
Refused edits retain their reasons, including `RestartRequired` and `Unproven`. Conflict attribution distinguishes
**Certain**, **Named from journal**, **Best-effort drift**, and **Unknown**; best-effort detection does not prove who
wrote a value or that no other writer exists.

A missing file is different from a valid snapshot with no edits. Unreadable, malformed, unsupported and oversized
reports show an error with **Refresh report** available. A stopped game, a report older than the current process start,
an old snapshot (over 24 hours), or an unverifiable process start/clock is marked **Stale snapshot**. Its saved values
remain readable. Windows report PIDs are not compared with host PIDs because Proton uses a different PID namespace.
The page refreshes the snapshot periodically and also offers an explicit refresh.

## Read-only launcher command

`registry_report_read({ gameId: string })` returns a UTF-8 string, or `null` when no file exists, using the launcher's
normal structured error on I/O failure. The caller cannot supply a path. The native reader resolves the known game
folder, refuses non-regular files and paths outside it, and caps both the metadata size and actual read at **8 MiB**.
The frontend separately caps bytes before parsing, JSON nesting at 48, and containers (objects and lists) at one per 16 bytes of the byte cap (524,288); separators are not counted, so any real report up to 8 MiB parses.
Unknown format-1 members are ignored; unsupported formats and malformed consumed rows are errors.

The view retains at most 500 rows per report section, 64 final writers per field, 2,048 characters per text value,
and bounded list/record previews (16 members, depth 4, 128 visited values). Both native omission counts and launcher
omissions are disclosed. Sections paginate at 25 rows; the field filter searches only retained rows. Formula inputs
are shown in evaluation order. This is intentionally not an export of the full history.

All report text is rendered as React text, never HTML. Mapping records, hashes, raw member paths, game addresses and
function metadata are not presented. Rows the runtime marks `raw` (Advanced-tier raw paths: field rows, conflict lines
and refused edits) keep their outcome but hide the path. Other field names are checked against Kiln's published
registry identifier lock: a name it does not know (a misspelled field, or a field of a newer Kiln) is shown as written,
marked "Field unknown to this launcher", and a refusal keeps its native reason, which is what the mod author can act on. No game client is launched by this page.
