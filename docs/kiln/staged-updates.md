# Staged updates (0.2.0-dev.30.9)

A running server can't replace the DLLs it has loaded. A mod or Kiln update is therefore **staged** while the server
runs and **applied at the next start**, before anything loads it. The previous version is kept, so a rollback is
possible. Kiln.Admin's `update_*` ops stage, list, cancel and roll back
([launcher/admin-protocol.md](launcher/admin-protocol.md) section 8). Kiln Web Admin uses those ops after it has
downloaded an update and checked its signature. Kiln itself never downloads anything for this.

Staged updates work on dedicated servers only. A game client ignores `kiln/staged`.

## When it happens
| Start step | What it applies | Where |
|---|---|---|
| The proxy's entry gate (`dinput8.dll`), before it loads `kiln/kiln-core.dll` | unit `kiln`: a staged Kiln update, a requested rollback, or an automatic rollback | `native/proxy/dinput8.cpp` `apply_staged` |
| kiln-core, right after `kiln.log` opens, before `kiln.json`, the profile or any native mod is read | every `mod.<folder>` unit (folders under `kiln/mods`) | `native/core/src/boot.cpp` `staged_boot` |
| Kiln.Host, when the mods load at the server world's first step | the verdict: confirms, or marks a failure for the next start | `managed/Kiln.Host/StagedProbation.cs` |

Both steps use the same code: `native/core/src/staged.cpp` (`staged.h`). The proxy writes its lines to
`kiln/logs/proxy.log` and passes them to kiln-core in `kiln/staged/proxy-lines.json`. kiln-core writes them into
`kiln.log` with the prefix `proxy: staged:`. Its own lines start with `staged:`. A Kiln proxy older than 30.9 does not
apply Kiln updates. kiln-core then logs a warning that a Kiln update is still waiting.

## Folders (`<server>/kiln/staged/`)
| Path | What | Written by |
|---|---|---|
| `incoming/<anything>/` | sources a caller prepares (the files of a mod, or an unpacked Kiln runtime bundle) | the caller (Kiln Web Admin) |
| `mods/<folder>/staged.json` + `files/` | a staged mod update: the next contents of `kiln/mods/<folder>` | `update_stage` |
| `kiln/staged.json` + `files/<path>` | a staged Kiln update: the files that change, at their paths from the server folder | `update_stage` |
| `previous/mods/<folder>/`, `previous/kiln/` | the version the last change replaced (`kept.json` + `files/`): what a rollback puts back | the start that applied it |
| `rollback/<unit>.json` | a requested rollback, applied at the next start | `update_rollback` |
| `probation/<unit>.json` | the verdict on an update applied at a start: `pending`/`loading`/`failed` (a mod), `applied`/`booting` (Kiln) | kiln-core, the proxy, Kiln.Host |
| `tx/<unit>/journal.json` + `prev/` | a change in progress | the start that applies it |
| `attempts/<unit>` | how many starts tried a change that was interrupted or failed | the start |
| `results/<id>.json` | one record per change, or refusal (the newest 50 are kept) | the start |
| `.stage-<random>/` | `update_stage` building a unit before it renames it into place; kiln-core removes any left at the next start | `update_stage` |

Units are `kiln` and `mod.<folder>`. A folder name is a single path segment of at most 64 bytes that does not start
with `.`. Nothing in this process reads or writes `kiln/data`: no source comes from there, no change, kept version or
rollback touches it, and no plan, kept version or journal that names a path under it is followed.

## Formats
`staged.json` (format 1):
```json
{"format": 1, "kind": "mod", "folder": "kiln-webadmin", "id": "kiln-webadmin", "version": "1.2.0", "from": "1.1.0",
 "staged": "2026-11-23T10:00:00.000Z", "by": "kiln-webadmin: alice", "label": "nexus 1234 file 5678",
 "files": [{"path": "kiln.mod.json", "sha256": "<64 hex>", "size": 123}, ...]}
{"format": 1, "kind": "kiln", "version": "0.2.0-dev.30.10", "from": "0.2.0-dev.30.9", "staged": "...", "by": "...",
 "label": "...", "bundleSha256": "<64 hex>|null", "base": {"receipt": "<sha256 of kiln/.install.json>|null"},
 "files": [{"path": "kiln/kiln-core.dll", "sha256": "...", "size": 1}, {"path": "dinput8.dll", ...}, {"path": "kiln/.install.json", ...}],
 "remove": [{"path": "kiln/services/old.dll", "sha256": "<what Kiln installed>"}]}
```
- A mod's `files` are the whole new folder. A Kiln update lists only the files that change and the new receipt.
- Paths: relative, separated by `/`, valid UTF-8, at most 240 bytes. Refused: an empty, `.` or `..` segment, a
  leading `/`, a backslash, `: * ? " < > |`, control characters, a segment that ends in `.` or a space, and Windows
  device names (`CON`, `NUL`, `COM1`, ...). A Kiln update may write `dinput8.dll` and `kiln/...` only. It may never
  write `kiln/staged`, `kiln/data`, `kiln/config`, `kiln/logs`, `kiln/state`, `kiln/backups`, `kiln/control` or
  `kiln/crash-reports`. The comparison ignores letter case. The shared cases are in
  `schema/fixtures/staged/paths.json`, and the C# and the native tests both check them.
- A part of a path that exists must be named by its own name. Windows also reaches a file or folder by its 8.3 short
  name (`CRASH-~1` for `crash-reports`, `KILN-A~1` for `kiln-admin-console`, on volumes that make them, as most system
  drives do), which would pass the rules above. Such a path is refused when the update is applied, and Kiln.Admin
  refuses a mod folder named that way.
- `kept.json`: `{format, unit, version (the kept one; "" = it was not installed), replacedBy, kept, reason
  (update|rollback|auto-rollback), entries: [{path, before: "present"|"absent"}], receipt (Kiln only: the SHA-256 of
  the kiln/.install.json the change left in place, "" when it left none)}`.
- `results/<id>.json`: `{format, id, unit, op (update|rollback|auto-rollback), ok, version, from, error?, note?,
  finished, by (proxy|core), kiln, pid}`.

## Applying, and what a crash leaves
When a start applies a unit, it checks first and changes nothing until every check has passed:
- `staged.json` is read and every path in it is checked.
- Every file must be in the manifest. Each file's size and SHA-256 must match it.
- No link may sit in the staged files. No link or file may sit on the way to the target.
- A mod's `kiln.mod.json` id must match.
- For a Kiln update, the receipt must still be the one the plan was made from (kilnctl or the launcher may have
  changed Kiln since).

A refused update is removed, its reason is logged, and a result record is written.

Then one transaction runs:
1. Write `tx/<unit>/prev/kept.json`, then the journal (phase `applying`).
2. For each step: rename the current target into `tx/<unit>/prev/files`, then rename the staged file or folder into
   place. A mod is one step (the folder). A Kiln update is one step per file, with `dinput8.dll` second to last and
   `kiln/.install.json` last. Removals go into the kept version too.
3. Set the journal's phase to `committed`. Then tidy up: `tx/<unit>/prev` becomes `previous/<unit>`, what was staged
   (or the rollback request) is removed, and the probation and the result record are written.

All of these are renames inside the server folder. They never replace anything and copy nothing. The journal and the
records are written to a temp file, flushed and renamed. A start that finds a journal finishes it (phase
`committed`) or puts every step back (phase `applying`). What it puts back is read from which of target, backup and
source exist, so a crash while putting back is safe too. In every case the unit is the old version or the new one,
never a mix. A change that is interrupted or fails three starts in a row is dropped and logged. A journal that can't
be read, or whose paths are not its unit's, is left alone with an error.

A mod folder switched off with `.disabled` stays switched off. The folder is replaced whole: a mod keeps its own data
in `kiln/data/<id>` and `kiln/config`, which this does not touch.

## Free space
A full disk would cost the world its next save. The world's savegame folder is the one `enshrouded_server.json`'s
`saveDirectory` names (by default `<server>/savegame`), and both checks below ask the volume that holds `kiln/staged`.
- `update_stage` copies only when the volume still has 2 GiB plus the size of the world's savegame folder free after the
  copy. Otherwise it refuses before it copies anything, and the error says how much is needed and how much is free.
- At the start, a staged update is applied only while that volume has 2 GiB plus the savegame folder's size free. The
  apply itself takes no room (it only renames), so this needs no room for the files. With less free, the update waits:
  it stays staged and kiln.log says so (`staged: <unit>: the staged update to <v> waits for free space: ...`). It
  doesn't count as a failed attempt, and the next start tries again.
- Rollbacks never wait for free space: they bring back the version that worked.
- When the volume can't be asked, nothing is held back.

Probation records are written like the journal: a temp file, flushed to the disk, then renamed over the record. That
holds for the ones kiln-core and the proxy write and for Kiln.Host's (`loading`, `failed`). A power cut leaves the
record that was there before or the whole new one, never an empty file.

## Rollback, and the automatic one
- `update_rollback` writes `rollback/<unit>.json`. The next start swaps the current version with the kept one, so the
  version it rolled away from becomes the kept one.
- A Kiln rollback, requested or automatic, needs Kiln to be the install the change left: `kiln/.install.json` must
  still be the receipt `kept.json` names. The kept version holds only the files its change changed, so put back over a
  Kiln that kilnctl or the launcher installed since, it would make a mix of two versions. Such a rollback is refused
  (Kiln.Admin refuses the request; the start logs why and drops the request or the probation).
- **A mod**: kiln-core writes `probation/mod.<folder>.json` as `pending`. When the folder loads, Kiln.Host sets it to
  `loading` right before the mod's code runs. If the mod loaded, Kiln.Host removes the record. If it did not, Kiln.Host
  sets `failed` with the reason. Only the mod's own failure counts: `dependency X ... did not load` does not. The next
  start rolls back a `failed` or `loading` mod (`loading` means that start ended while the mod loaded) and logs:
  `staged: mod folder <f>: it failed to load right after its update to <v>: <reason>; rolled back to <old> (once: Kiln
  does not roll back the version it went back to)`. `pending` waits: the server stopped before the mods loaded, or the
  folder is switched off.
- **Kiln**: the apply writes `probation/kiln.json` as `applied`, and the proxy sets it to `booting` just before it
  loads the new kiln-core. Kiln.Host removes it after the mods have loaded. With `"clr": false`, kiln-core removes it
  at the end of its boot. If the next start finds `booting`, it rolls Kiln back. That covers a crash, a failed boot,
  safe idle, and also a stop before the mods load (the server world's first step): a restart while the world still
  loads, and Kiln's own deliberate stops before any world loads (a required boot mod, the NPC roster, the native
  content build: exit codes 19201 to 19203), even when a mod caused them.
- The version an automatic rollback goes back to has no probation, so it never loops.

## Limits
- Native mods (`kiln/native/<id>`) and other engines' mods are not staged units.
- `kiln/mods/<folder>` folders that Kiln's receipt lists (Kiln.Admin, for example) are updated with Kiln, never on
  their own.
- A server installed from the server pack (unzipped by hand) has no receipt. Its first staged Kiln update installs the
  server's default components and does what the manual update in [dedicated-servers.md](dedicated-servers.md#updating-kiln)
  does: `kiln/dotnet`, `kiln/host`, `kiln/services` and `kiln/sigdb` end up exactly the new version's. It also writes
  `kiln/.install.json`. Its `dinput8.dll` must carry a Kiln proxy marker.
- The proxy grew from about 130 KB to about 1.1 MB in 30.9 (std::filesystem, JSON and SHA-256 for this step).
- The automatic rollback of a Kiln update runs in the new `dinput8.dll`. A new proxy that cannot start at all rolls
  nothing back: copy `kiln/staged/previous/kiln/files/dinput8.dll` over `dinput8.dll` (copy, don't move), and the next
  start rolls the rest back.
- `update_*` work on a dedicated server only: on a game client (one that hosts a session may run Kiln.Admin),
  `update_stage` and `update_rollback` are refused. Kiln.Admin finds `kiln/` next to the server's exe, as the start
  does, wherever a profile keeps the mods' data.

Tests: `native/core/tests/test-staged.sh` (host, ASan+UBSan: every crash point of five scenarios in forked children,
a second crash while putting back, refusals, rollbacks, `kiln/data` byte for byte), `test-staged-wine.sh` (the same
on the Win32 code path under Wine; the same binary runs natively on Windows, where 8.3 short names are checked
against a real NTFS volume), `native/proxy/tests/test-staged-gate.sh` (the real `dinput8.dll` under Wine, or
`staged_gate/gate_test.py` natively on Windows: a stand-in server exe whose loader maps the proxy, which renames the
running `dinput8.dll` and the folder that then holds it), `managed/tests/Kiln.Host.Tests` `StagedProbationTests` and
`StagingTests`.
