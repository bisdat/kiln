# Kiln changes to shroudlink

`upstream/` is [bjck/shroudlink](https://github.com/bjck/shroudlink) v0.8.0 (commit `bfeebd07`, MIT, `LICENSE`):
the crates `server`, `common`, `link` and `sdk` from its `mod/` workspace, and its `Cargo.lock`. Every change is
marked `kiln:` in the code. Without the cargo feature `kiln` the server crate builds as upstream's
`steam_api64.dll` proxy again; `../build.sh` builds it with `--features kiln` as `kiln-shroudlink.dll`.

| File | Change |
|---|---|
| `server/src/kiln.rs` | new (Kiln's own, MIT): `kiln_plugin_early` (EML data mods to kiln-eml), `kiln_plugin_main`/`on_ready` (profile mod list, step aside, start), log mirror, `kiln/state/shroudlink.json`, host tests |
| `server/Cargo.toml` | feature `kiln` |
| `server/build.rs` | with `kiln`, the Steam stubs are not exported |
| `server/src/lib.rs` | with `kiln`: no `DirectInput8Create`/crash-handler exports, no steam proxy, no start from `DllMain`, Kiln's crash handler kept; `init` split into `start(load_mods)` returning whether this copy claimed the process |
| `server/src/plugins.rs` | the loader reads several roots (folders or single DLLs) with a chosen shadow folder (`load_from`); `OUTCOMES` records each DLL's result; `touch_for_test` searches the roots |
| `common/src/log.rs` | `set_sink`: every line also goes to Kiln's log |
| `common/src/scan.rs` | `find` falls back to `.text` as the exe file holds it when the live code no longer matches (another hook, e.g. Kiln.Gameplay's on the event dispatcher, already replaced the prologue), so shroudlink's hook chains onto it |

Review corrections (all integration changes, marked `kiln:`; upstream runtime callbacks and game APIs unchanged):

| File | Change |
|---|---|
| `server/src/kiln.rs` | Validate API and info sizes before accessing full structs or writing fields; require ABI/service versions and required non-null slots. EML requests and accepts the v1 prefix, including larger v2 tables. Lock mod paths through reading/copying, reject traversal/aliases/reparse points from the authorized root down, compare final handle paths, and apply the same discovery policy to DLLs and EML folders. Core detection also requires shroudlink's own strings; export names alone are ambiguous. ABI, EML, path, proxy-identity and existing-detour regression tests. |
| `server/src/plugins.rs` | With `kiln`, validate sources again on hot reload, keep paths locked while copying, check the shadow root before cleanup/writes, exclusively create new shadow files, restrict dependency search, and catch native initialization faults without unloading possibly active modules. Shadow-overwrite/link and SEH regression tests. Non-Kiln loading retains upstream's copy/load behavior. |
| `server/Cargo.toml`, `server/build.rs` | Compile the Kiln-only SEH bridge in `../../src/guard.c` using the already pinned `cc` build dependency. The workspace lock lists that dependency. |
| `common/src/scan.rs`, `common/src/file_scan.rs` | Bound and validate the exe-file fallback's PE headers, architecture, executable `.text` identity, section table and raw data; reject empty patterns. The new parser and malformed/truncated-file tests can also run directly with `rustc --test` without Windows. |

Launcher server sync (marked `kiln:`):

| File | Change |
|---|---|
| `server/src/kiln.rs` | `roots` also takes the direct subfolders of `shroudlink_mods/` (`mod_folders`: not hidden, not an EML folder, no links), where the launcher's server sync puts each mod (`shroudlink_mods/<id>/`). Discovery keeps the outermost listed root as its path-check boundary; the running loader retains checked directory handles for its automatic dropin root from before discovery until process exit, preventing root/ancestor replacement between discovery, copying and hot reload while allowing DLL updates. Handles use `OwnedHandle` for ownership and cross-thread transfer. Discovery deduplicates DLLs by volume/file identity and exact contents, including hard links and separately copied/renamed DLLs, with the first profile/dropin occurrence winning. Content comparisons hold checked paths and use bounded buffers for equal-sized files; distinct DLLs sharing a filename remain separate. Profile order, then sorted flat DLLs, then sorted subfolders and their DLLs determine load order. Overlapping-root/copy/hard-link, directory-pinning/update and replaced-dropin-root regression tests. Upstream's own core still reads only `shroudlink_mods/*.dll`. |

Own loader and drop-ins (dev.22, marked `kiln:`):

| File | Change |
|---|---|
| `server/src/kiln.rs` | With an active profile `shroudlink_mods/` is read only with drop-ins on for shroudlink (`"dropins": true` or a list naming `shroudlink`), whether or not the profile lists shroudlink mods (before, a profile without a `mods.shroudlink` list read it too). While standing aside (a real core runs, or another core claimed the process), `kiln/state/shroudlink.json` lists every enabled profile entry: `own-loader` when its DLL is a file directly in `shroudlink_mods/` (the only place upstream's core reads), else `not-loaded` with the reason, and each `not-loaded` entry is logged at WARN. Host test `standing_aside_reports_profile_entries_truthfully`. |

The step-aside hint (dev.30, marked `kiln:`):

| File | Change |
|---|---|
| `server/src/kiln.rs` | `aside_entry` names the core file `real_core_in_process` found in the `not-loaded` reason, and `shroudlink.json` carries that path in `corePath`. Advice names that file, including a core under `kiln/chain/`, and keeps the client addon for its MODS tab. With none known (another core claimed the process), the reason lists possible names. Host test `standing_aside_reports_profile_entries_truthfully`. |

The chest-name scan (0.2.0-dev.29.19, marked `kiln:`; a player's report: with the assistants mod on, a busy area
stuttered at every scan on a 4-thread CPU with the page file on a hard drive):

| File | Change |
|---|---|
| `server/src/names.rs` | A scan reads only the pages in the working set (`K32QueryWorkingSetEx` per 1 MiB chunk): a record on a skipped page stays as the scan before saw it (`keep_skipped`; such a page was not written since it was paged out), and the scan no longer pages the game's memory back in. The background thread runs in `THREAD_MODE_BACKGROUND_BEGIN` and rests 4 ms after each 4 ms of work; a scan the game thread waits for (`lookup_fresh`) does not rest. The log line says what a scan read and skipped and how many records it kept. Host tests `ranges_and_runs`, `records_on_skipped_pages_are_kept`, `a_scan_finds_a_record_in_this_process`. |

The chest-name index, steady state (0.2.0-dev.29.22, marked `kiln:`; the same player on 29.21: with nearly all of the
game's 5.9 GB of private memory resident, each background scan read all of it in 90-155 s with its rests, the next began
15 s after it ended, so it never stopped, and an unknown key ran the whole sweep on the game thread):

| File | Change |
|---|---|
| `server/src/names.rs` | No sweep on a timer. A name key is the FNV-1a 64 of its text, so a record whose text hashes to its key stays in the index for good (`known`); a renamed object carries a new key. A sweep runs only for a key that is asked for and not known (`lookup_known` / `lookup_fresh` -> `want`): first a warm sweep of the 4 MiB windows around the records found before, then (background) a whole sweep that reads the allocations that held records first and stops when the asked keys are found. Asked whole sweeps start at least 30 s, and at least the last whole sweep's duration, after it ended (`ask_gap`); a key a whole sweep missed is not asked for again for 5-30 min (`miss_retry`). The game thread runs at most a 32 MiB warm sweep (before: the whole sweep). Periodic sweeps only while the name component is not known by its type hash (4x the last duration, doubled per quiet sweep, 30 s to 30 min, a quarter of a core); asked sweeps half a core. Records whose text does not hash to their key keep upstream's address rules and are re-read every 15 s (`verify`). One log line per sweep (kind, read, skipped, kept, stopped early). Host tests `schedule_sweeps_only_when_asked_or_learning_and_never_back_to_back`, `unknown_keys_are_asked_for_once_and_a_missed_one_waits`, `merge_keeps_hashed_names_for_good_and_counts_new_ones`, `name_keys_are_fnv1a_64_of_the_text`, `warm_and_asked_sweeps_read_near_known_records_first`, `benchmark_whole_against_warm_sweep` (512 MiB synthetic heap: whole sweep 108 ms, warm 44 us, asked 256 us under Wine). |
| `server/src/game_api.rs` | The `CustomPlayerString` component's index is found once by its type hash (`components::index_of`, `0x0e699564`), so the index needs no sweep to learn it (the learning path stays as the fallback and turns periodic sweeps on). With `kiln`, a named object's text comes from kiln.items `container_name` first (the game's string table, no sweep), accepted only when its FNV-1a 64 is the object's key; the index answers when Kiln cannot (no service, feature off, not the tick thread). An unknown key from `name_known` is handed to the index (`lookup_known`). |
| `server/src/kiln.rs` | `container_name`: kiln.items v3 table up to `container_name`; logs once which source answers chest names. |
| `server/src/plugins.rs`, `server/src/lib.rs` | `LoadLibraryW` and `proxy` imported only without `kiln` (no unused-import warnings in the Kiln build). |

Staleness: a name Kiln reads is current at every ask. From the index: a known key's text never changes; a new or
renamed object's key is found at the ask when its record lies near known ones, else by the background within
`ask_gap` (30 s, or the last whole sweep's duration) plus the time an early-stopping sweep takes to reach it.

The enclosing workspace enables `Win32_Storage_FileSystem` and `Win32_Security` for those checked handles.
`../build.sh --test` also includes the common crate's tests. `../src/guard.c` and `../tests/nearby/` are Kiln's own
integration code/tests outside the vendored crates.

Updating: copy the four crates and `Cargo.lock` of the new tag over `upstream/`, re-apply the `kiln:` changes (they
are small and local), run `../build.sh --test` and the lab self-test (docs/kiln/engines/shroudlink.md).
