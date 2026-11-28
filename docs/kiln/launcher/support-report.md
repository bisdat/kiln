# Support reports

When mods don't load, one file answers the questions a maintainer would otherwise ask over several posts: which Kiln,
which OS and Proton, which game build, what is in the game folder, what the profile loads and what the last start did.

- **Kiln Launcher:** Logs, "Support report" (above the tabs; also with no game found yet, and on a server's Logs page).
  "Save report..." first shows "What's in this report" (below), then asks where to save `kiln-report-<date>.zip`;
  "Copy summary" copies a few lines for a forum post.
- **kilnctl** (servers, manual installs): `kilnctl report [NAME] [--dir D] [--out FILE] [--print] [--contents]
  [--exclude PATH|other-mods ...]`. `NAME` is an instance; `--dir` a server or client folder; neither: the client
  Steam knows. `--print` writes `summary.txt` to the terminal instead of the zip; `--contents` prints what the report
  holds (each file by group, its size and what was replaced in it, as "What's in this report"); `--exclude` leaves
  out another mod's file by its path in the zip (`game/BuildingTools.log`, repeatable) or `other-mods` for all of them,
  and refuses Kiln's own files with why.

Both only read. Nothing is written to the game folder, Steam's files are only read, the game may be running.

Code: `crates/kiln-manage/src/report.rs` (`build`, `redact`, `write_zip`, `contents`), the Tauri commands `report_save`,
`report_preview` and `report_summary` (`launcher/src-tauri/src/commands/logs.rs`),
`launcher/src/components/SupportReport.tsx` and `ReportContents.tsx`, `runner/kilnctl/src/report.rs`.

## What's in this report

(dev.29.22) A player declined to send a report because it held "a little too much personal information". Before a
report is saved or sent the launcher now shows what it holds, built exactly as it will be written (`report_preview`,
`report::contents`; nothing is written):

- the files, grouped as **Kiln's logs** (summary.txt, report.json, Kiln's logs, the crash report, the launcher's logs),
  **other mods' logs** (every file `report::excludable` says is another mod's or tool's: Building Tools, ShroudLink,
  Auto Loot Plus, XHL-Core, Greeneye, OptiScaler's log and settings, and any file `report::file_kind` does not know),
  **settings** (Kiln's state files, the profile, chain.ini, kiln.json, the receipt) and **folder listings**;
- each with one plain line on what it holds (`file_kind`'s name, the launcher's `reportContents.about.<name>`), its
  size, and how often each replacement stands in it ("Replaced: your home folder (169 times), player names (3
  times)"; summary.txt's own "Left out" section, which names every token once, is not counted);
- **View summary.txt**, and the "Never in a report" list (`report::NEVER_COLLECTED`, the items of summary.txt's "Left out");
- a **Leave out** box on each other mod's log, and one for all of them. Kiln's own files, the settings and the listings
  have none: support needs them to see what went wrong (the group says so). Redaction cleans every file the same
  way, but it cannot know everything another mod writes, which is why those are the ones a player may leave out.

`ReportInput::exclude` takes zip paths or `other-mods`; names of files that cannot be left out are ignored. A file
left out is still read and redacted (for the preview, `Report::left_out`) but is never in the zip and takes none of the
6 MiB budget; summary.txt lists it under "Left out by the player (other mods' own files)" and report.json as
`leftOut`.

- **Save report...** opens "What's in this report" as a dialog: Save (then the file dialog), Cancel (nothing written).
- **Send to Kiln support**: the check step says what the zip holds ("1 file left out. Replaced: ...") and "What's in
  this report" opens the same view inline; a change prepares the report again (`community_report_prepare` with
  `exclude`), and Send waits for it, so it always sends the report as the boxes are.
- **Steam Deck**: the check step's "What's in this report" row opens the list with the pad: each file a row, A leaves
  another mod's log out or puts it back, "View summary.txt" opens it in a scrollable dialog.

## What is in the zip

| Path | What |
|---|---|
| `summary.txt` | readable in a post: findings, then the system, the game, Kiln in the game, the profile, the last start, sections that could not be read, the file list, what was left out |
| `report.json` | the findings and the unreadable sections as JSON (`{format, written, tool, findings: [{id, level, text}], errors}`) |
| `game/kiln/logs/kiln.log` | the last 1 MiB |
| `game/kiln/logs/proxy.log`, `game/kiln/logs/eml.log`, `game/shroudlink_client.log` | the last 192 KiB of each, when present (a link in its place is not followed: the report says "a link, not followed"); the shroudlink addon's own log gets the same redaction as Kiln's logs and nothing more (see below) |
| `game/kiln/logs/tick-stats.jsonl` | the last 1 MiB (whole lines), when present: Kiln's step times per world and minute, rewritten at every game start (since dev.29); since dev.29.19 also each world's impact memory (`impact`; kiln.log warns at 50/75/90 % and names the programs that fill it: [../services/abilities.md](../services/abilities.md#impact-memory-watch)) |
| `game/XHL-Native-Plugin-Host.log`, `game/kiln/chain/<id>/XHL-Native-Plugin-Host.log` | (dev.29.18) XHL-Core's own log, when present, next to the DLL it runs as: the game's `dinput8.dll` (its author's instructions) or Kiln's copy of it for a Native mod of the profile (`kiln/chain/<id>/xhl-core.dll`, at most 4 such folders, each a Kiln mod id): which plugin folders it loaded, skipped or refused. The last 192 KiB of each, redacted as Kiln's logs; a link in the file's or the folder's place is not followed |
| `game/kiln/logs/greeneye-loader.log`, `game/greeneye/loader.log`, `game/greeneye/loader.second-copy.log` | kiln-greeneye's log and Greeneye Loader's own, the last 64 KiB of each, when present |
| `greeneye-mods/<id>/game/*.log`, `.../game/*.status`, `greeneye-mods/<id>/library/...` | each Greeneye mod's own log and status files, one zip folder per mod folder (`game`: its folder in `greeneye/`, `library`: a Library entry; one id may have both) (top level of its folder, at most 4 per mod and 12 mods, the last 64 KiB of each): a Greeneye mod writes what it found and patched only there (The Worst Map Mod: `unlock_map_markers.log`, `UnlockMapMarkers.status`). The folders are those `kiln/state/greeneye.json` names and those in `greeneye/`, read only inside `<game>/greeneye/` or the launcher's `library/greeneye/` (resolved, no links); any other folder is left out with a note (`greeneye-log-skipped`). Compared resolved, so a different spelling of the same folder is the same. Its settings (`.ini`) and data are not read |
| `game/OptiScaler.ini`, `game/OptiScaler.log` | (dev.29.18) only with OptiScaler in the game folder (`engines::optiscaler_in`): its settings as they are, redacted, at most 1 MiB, and the last 192 KiB of its own log when it writes one (`[Log] LogToFile=true`), each under its name on disk |
| `game/kiln/crash-reports/<newest>` | the last 64 KiB of the newest crash report |
| `game/BuildingTools.log`, `game/BuildingTools.prev.log`, `game/shroudlink.log`, `game/auto-loot-plus.log` | (dev.29.19) other mods' own logs in the game folder, when present: Building Tools' (this start and the one before), ShroudLink's core, Auto Loot Plus' native part. The last 192 KiB of each, redacted as Kiln's logs, added after Kiln's own logs and the crash report; a link in the file's place is not followed. Their settings (`BuildingTools.ini`) are not read |
| `game/kiln/state/*.json` | every state file (boot, mods, native, last-server, hooks, each engine's), at most 1 MiB each |
| `game/kiln/profile.json`, `kiln/chain.ini`, `kiln/kiln.json`, `kiln/.install.json`, `dinput8-chain.ini` | as they are, redacted; a link in a file's place, and a state file that is a link, is not followed ("a link, not followed") |
| `launcher/launcher.log` | the last 192 KiB of `<data>/logs/launcher.log` |
| `launcher/game-start.log` | (dev.29.17) a game that starts directly: the last 192 KiB of `<data>/logs/game-start-<installId>.log`, the umu-run, Proton or Wine output of its last direct start ([custom-game-folders.md](custom-game-folders.md)) |
| `listings/game-folder.txt` | the game folder's top level: type, size, modified, name |
| `listings/kiln-folder.txt` | `kiln/`, two levels deep |
| `listings/mod-folders.txt` | each engine's mods folder (`kiln/mods`, `kiln/native`, `kiln/plugins`, `emf/mods`, `kiln/eml/mods`, `mods`, `greeneye`, `shrouded_engine_tweaks/mods`, ShroudForge's, `shroudlink_mods`, `ember/mods`), two levels deep |

All files together are cut at 6 MiB before compression (the zip is usually well under 1 MiB). A listing stops at 2000
lines. A file that is cut says so in its first line.

### summary.txt

- **System:** OS (Windows edition, version and build from the registry; on Linux `/etc/os-release` and the kernel), a
  Steam Deck (the launcher's own detection; kilnctl: DMI `Jupiter`/`Galileo`, SteamOS, `SteamDeck=1`). The Linux
  launcher adds how it draws its window (`Launcher:`) and (dev.29.18) WebKit's sandbox in this start and the probe of
  bwrap (`WebKit sandbox:`).
- **Game:** the folder (redacted), client or server, platform, Steam build, the Proton prefix, the compat tool from
  Steam's `config.vdf` and the prefix's `version` file, Steam's launch options (not for a direct start), running or not,
  the exe's sha256 and whether `kiln/sigdb/builds/<side>/<sha256>.json` says Kiln verified this build. (dev.29.17)
  "Custom folder: yes" for a folder no Steam library knows, and "Start:" through Steam, or directly with the umu-run,
  Proton or Wine (and umu's Proton), whether it was automatic or the player's, and its `WINEDLLOVERRIDES`. A direct
  start reads nothing of Steam's: no compat tool (the prefix's Proton `version` instead), no launch options, and a
  start that did not load Kiln points at `launcher/game-start.log`, not at Steam's launch options.
- **Kiln in the game:** the install receipt, each DLL under a proxy name next to the game and what it is
  (`engines::scan_proxies`), `kiln/chain.ini`.
- **Profile:** `kiln/profile.json`: name, generation, drop-ins, each mod (kind, id, version, on/off).
- **The last start:** `boot.json` (Kiln, side, hooks, policy, profile mode, safe idle, chain, plugins, errors),
  `mods.json` and `native.json` (each mod's state and reason), each engine plugin's state file (its state word and
  each mod's state and reason, read as on Logs > Engines), the last boot in kiln.log (`logs::boot_state`), the newest
  crash report.

### Findings

Each is one plain sentence. `problem` explains why mods don't load, `warning` may, `note` is context.

| id | When |
|---|---|
| `not-installed` | no install receipt and no `dinput8.dll` (problem); Kiln's `dinput8.dll` without a receipt (note) |
| `proxy-not-kiln` | `dinput8.dll` is another engine's or an unknown DLL, with or without Kiln's receipt |
| `files-missing` | files of the receipt are gone (names them; the usual cause is an antivirus quarantine) |
| `files-changed` | files of the receipt have another sha256 (`install::modified_files`) |
| `install-blocked` | (dev.29.18, inbox 92) a problem: an install, update, Repair or rollback of Kiln in this game folder failed because Windows refused one of Kiln's DLLs or exes (access denied, os error 5, or a sharing violation, os error 32: an antivirus or another program; Windows Security's virus errors 225 and 226; `blocked::last_in_log`), as launcher.log says, and no install worked since: no later install, rollback or uninstall is done in the log, and the receipt is older than that failure or missing. A later refusal (the game was running) leaves it standing. Names the task, the time and the file. A launcher since 0.2.0-dev.29.18 writes the error's `{"blocked": ...}` detail into that line; an older launcher's line is read by its words (the path in the game folder and the os error) |
| `not-loaded` | the game runs and boot.json is not from this start (`launch::boot_is_current`); or the last launcher start (`<data>/installs/<id>.launch.json`) is newer than boot.json; or the game has a log but Kiln wrote neither proxy.log nor boot.json. Under Proton without the dinput8 override the sentence names the launch option (`game::launch_options_info`) |
| `proton-override` | Kiln is in a Proton game, nothing says it failed, and Steam's launch options have no dinput8 override |
| `unknown-build` | no build record for the exe's sha256 (problem: "Kiln doesn't know this game build (<12 hex digits>). Kiln starts with the game but stays idle: no mods load.", the launcher's words on Play and in Settings) or one that is not verified (warning) |
| `safe-idle` | boot.json's `safeIdle` |
| `own-loader` | an engine plugin's state file says it stood aside (`phase: aside`, `mode: self-hosted`, `steppedAside`) |
| `loader-in-folder` | another engine's own loader under a proxy name the game loads (EML's `dbghelp.dll`, SET's `powrprof.dll` ...): a warning on Windows (it always runs), a note under Wine/Proton (only with an override). Left out when that engine's plugin already said it stood aside |
| `unlisted-mods` | mod folders in the game folder that the active profile does not load (drop-ins off for Kiln's kinds and EML; a `mods.<engine>` list for the compat-hosted engines; ShroudForge and SET always read their folders) and that the profile does not list itself |
| `sf-startup-failed` | ShroudForge's own record of its startup pass (`shroudforge/state.json` `assets`) says `failed` and is the last start's (`sf_startup::failure`: written in Kiln's last start by ShroudForge hosted or found there, not left by an earlier one): its words (redacted whole, then put on one line and cut), the switched-on mods that need the pass, and for the fault ShroudForge builds without the fix have under Wine and Proton (`os error 5`) what to change. The record's line is in summary.txt under "The last start" |
| `sf-mod-bundled-newer` | a note (`info` in JSON): a ShroudForge mod in `<game>/mods` has an older semantic version than the same exact, case-sensitive manifest id in `<game>/kiln/plugins/shroudforge/mods`, regardless of folder names. Text: `{mod_id} {installed_version} is older than the {bundled_version} Kiln brings.` Equal or newer versions and mods with no bundled copy are not findings. Missing or unreadable ShroudForge manifests are listed under "Not readable" instead; no mod files or switches are changed |
| `mod-failed` | `failed` in mods.json, native.json, boot.json's plugins, a chain entry `failed`/`missing`, or a mod in an engine's state file whose state says fail/error/refused; with the reasons |
| `hosted-mods` | a note: Kiln mods `skipped` in mods.json with `side 'server': loads while this client hosts a world`: they start with the player's own world (single-player or co-op), so `skipped` in the menu is expected (docs/kiln/client.md, "Mods and sides") |
| `hosted-off` | a warning: the same with kiln.json `"mods": {"hosted": false}`: they never start in this game |
| `crash` | there is a crash report (the newest is in the zip) |
| `profile-invalid` | `kiln/profile.json` does not parse, or the last start found it invalid |
| `newer-mods` | Kiln mods of the profile built against a newer Kiln (`game::newer_mods`, the Play page's "Update Kiln") |
| `optiscaler` | (dev.29.18) a note: OptiScaler is in the game folder (`game::graphics_tools`), with the file and what Kiln's overlay does next to it with its OptiScaler.ini (the Play page's words): steps aside with OptiScaler's own menu on or frame generation set, draws with `OverlayMenu=false`; (0.2.0-dev.31.25) drawn anyway, at the player's risk, while the game's kiln.json has `ui.overlay` "on" ("Draw Kiln's overlay anyway", `game::overlay_anyway`). summary.txt says the same in a "Graphics tool:" line under the proxies |
| `no-game` | the report was made without a game (note) |
| `webkit-sandbox` | (dev.29.18, from the Linux launcher: `ReportInput.findings`, `sandbox::finding`) the start's probe found that the bwrap WebKit runs cannot start (its line) or is missing: a problem when this start runs WebKit's sandbox (the launcher's page cannot open; turn it off in the launcher's message or with `--no-webkit-sandbox`), a note when it does not (a default start: then it is not why a window stays empty); the AppImage's own missing bwrap (it has none on any system) only when the start runs the sandbox. With the AppArmor value when it restricts user namespaces. summary.txt's system has a `WebKit sandbox:` line in every Linux launcher report ([app.md](app.md), "WebKit's sandbox (Linux)") |
| `launcher-files-missing` | (dev.29.18, inbox 92, from the Windows launcher: `ReportInput.findings`, `own_files::finding`) a warning: the launcher was installed by its setup (NSIS) and its own `uninstall.exe`, or its exe while it ran, is gone from its folder at this start: an antivirus may have quarantined it. Names the files; launcher.log has a `launcher:` line, and the window says it once |

The bundled-version check uses the shared manifest reader and semantic-version helper. Prereleases compare below
releases, numeric prerelease parts compare numerically, and build metadata does not affect precedence. Although the
version helper accepts a `v`/`V` prefix and fills missing minor or patch parts with zero, ShroudForge's manifest reader
requires three numeric parts without a prefix and accepts at most one prerelease or build suffix. Those refused forms,
non-semver versions, and numeric parts beyond the helper's signed 32-bit bound produce no version finding.

Only the two fixed folders above are checked, not a configured mods path elsewhere. Folder lookup follows Windows'
case-insensitive spelling under Proton too; manifest ids remain case-sensitive. Directory links, junctions and
manifest links are not followed. Identification uses manifests and Lua entry-point names, never INI or DLL contents;
a native-only package with no explicit ShroudForge manifest metadata is ambiguous and is left out.

## Privacy

Every text goes through one function, `report::redact`:

- the home folder becomes `<home>`, in Linux form (`/home/me`), Wine form (`Z:\home\me`), Windows form
  (`C:\Users\Me`), with `/` or `\` (also doubled inside JSON), case ignored;
- the machine name becomes `<host>` (not when it is the distribution's name, as on a Deck named `steamdeck`);
- the user name, the home folder's last component and the Steam account names (`AccountName` in each Steam root's
  `config/loginusers.vdf`, `AutoLoginUser` in `registry.vdf` under `~/.steam`, Flatpak's or Snap's `.steam`) become
  `<user>`, also inside other words (`me-pc`, Wine's `C:\users\me`); a name of 3 characters or fewer only as a whole
  word, a common word (`user`, `steam`, `kiln` ...) only inside the home folder; Steam display names (`PersonaName` in
  `loginusers.vdf` and in each `userdata/<id>/config/localconfig.vdf`) become `<steam-name>` (whole words: no letter or
  digit next to it, so `Name_world` loses it too). The Steam roots are those Kiln finds games in: native
  (`~/.local/share/Steam`, `~/.steam/steam`, `~/.steam/root`, `~/.steam/debian-installation`), Flatpak
  (`~/.var/app/com.valvesoftware.Steam/.local/share/Steam`, `.../data/Steam`), Snap, a Steam Deck's, and on Windows the
  registry's (`Redactor::add_steam_accounts`);
- other players: the names Kiln's and ShroudLink's own log lines give the players of a session (Kiln's players
  service `players: joined slot 1 entity 7 'Name' (steam ...)`, Kiln.Network `net: Name (0x81) joined with Kiln`, `peer
  Name ...`, `kick`/`kicking`/`banned`/`refusing Name`, `... sent to Name`, Kiln.Saves `Name has no character id`,
  ShroudLink's `client <id> is Name` and `is player 2 (Name)`) become `<player>` in those lines (`report::player_names`),
  and a name of 3 characters or more, read from kiln.log and shroudlink.log before anything is written, also as a whole
  word in every other line and file (`Redactor::add_players_from`: a mod's own `welcome back, Name`). The player whose
  Steam name is in `loginusers.vdf` stays `<steam-name>`;
- SteamID64s, `userdata/<n>` and `[U:1:<n>]` become `<steamid>`; IPv4 addresses other than `127.x` and `0.x`
  (a version such as `Kiln.Host 0.2.0.0`) become `<ip>`; IPv6 addresses other than `::1` and `::` become
  `<ip>`, MAC addresses `<mac>`, mail addresses `<email>`;
- in every text (logs, INI files such as `OptiScaler.ini`, `chain.ini`), the value after a word that names a
  secret (`token`, `password`, `passwd`, `passphrase`, `secret`, `apikey`/`api_key`/`api-key`, `cookie`, `credential`)
  and `=` or `:` becomes `<redacted>` (`password=hunter2`, `ApiKey = abc`, `?token=abc&`, `token: abc`), and so does the
  argument of such a command-line flag (`-password hunter2`, `--apikey x`).

JSON files also go through `report::redact_json`: string and number values of fields named like a secret or an address
(`token`, `password`, `secret`, `apiKey`, `cookie`, `credential`, `private`, `auth`, `key`, `ip`, `host`, `endpoint`,
`nonce`, `*address`, `*url`, `*steamId`; also `api-key` and the names of people, `*userName`, `*accountName`,
`*personaName`, `*playerName`, `*characterName`, `*email`, `_` and `-` ignored) become `<redacted>`. A JSON file that does not parse or was cut loses the
values of such fields line by line.

Never read into the report: saves and world data (`kiln/data/`), `kiln/config/` and the profile's config folder (mod
settings, `network.json`), `kiln/data/kiln-admin/`, the contents of any mod (only names, sizes and times in the
listings, and ShroudForge manifest ids and versions in the bundled-version finding; manifests themselves are not
included), Steam's files other than `config.vdf`'s compat tool, the launch options and `loginusers.vdf` (for the
names to replace). `summary.txt` ends with this list.

What redaction cannot catch: a character or server name the player chose that appears in a log line other than the
ones above (`kiln/state/last-server.json` keeps the server's name on purpose), a player's name of 1 or 2 characters
outside the lines that name players, and 8.3 short names that cut the user name (`FROSTL~1`). `game/shroudlink_client.log` is written by the third-party shroudlink
client addon, not by Kiln (as `game/BuildingTools*.log`, `game/shroudlink.log` and `game/auto-loot-plus.log` are written by
those mods): its text gets only the redaction above, so whatever else the addon logs stays as it is
(server DNS or UNC host names, other players' names in lines other than ShroudLink's `client <id> is Name` ones,
secrets under names other than those above).

## When things are broken

The report never fails on the game's state. No Kiln, no game folder, a corrupt profile, an unreadable file, a huge log,
the game running: each section that cannot be read is listed under "Not readable" with why, and the rest is written.
Only writing the zip itself can fail (`report_save` rejects with the I/O error).
