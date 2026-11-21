# Kiln Launcher docs

| Doc | What |
|---|---|
| [contracts.md](contracts.md) | every format, path, flow, Tauri command, event, error code and crate module (the contracts) |
| [admin-protocol.md](admin-protocol.md) | KAP/1: Kiln.Admin over TCP (framing, handshake, ops, SSH tunnels) |
| [../dedicated-servers.md](../dedicated-servers.md) | for server owners: Kiln on a dedicated server (rented with FTP, own Windows PC, own Linux with kilnctl), checking it, admin, mods, game updates, removing it |
| [releasing.md](releasing.md) | versions, the launcher's self-update (sources, channels, signatures), the release folder and the steps to publish |
| [catalog.md](catalog.md) | the online catalog on Nexus Mods: the Kiln-mod rule, Nexus's rules for apps, adding and updates, Mod Manager Download (nxm://), tests, the real-API checklist; for mod authors: [catalog-authors.md](catalog-authors.md) |
| [custom-game-folders.md](custom-game-folders.md) | for players: a copy of the game no Steam library knows (an older build kept for mods): adding it, starting it directly without Steam (Windows; umu-run, Proton or Wine and the prefix on Linux), what Kiln does and doesn't do, unknown game builds |
| [support-report.md](support-report.md) | the support report (Logs, "Save report...", `kilnctl report`): what the zip holds, the findings, what is redacted and left out |
| [deck.md](deck.md) | the Steam Deck UI: how it is chosen, controls, screens, the Steam rule, tests and the Deck tester checklist |
| [pack-studio.md](pack-studio.md) | Pack Studio, the creator's tool: pack sources from a profile, settings captured, the pack layer's data editors, load order and conflicts, the test instance, credits, publish (link-only or public), server packs, kilnctl pack init/add/capture-settings/lint/credits/publish |
| [crucible.md](crucible.md) | Crucible packs (.kilnpack, format 2): the format, building from a profile, instances and their three layers, updates with a three-way merge, the server part, kilnctl pack, the API for the launcher UI and the Crucible service |
| [perf.md](perf.md) | what the launcher costs idle and per click, the caches and poll rules that keep it low, and the two harnesses that measure it |

Machine-readable contracts:
- `schema/`: JSON Schemas (`*.schema.json`, draft 2020-12), `examples/`, `fixtures/` (resolver, semver, manifest
  parity; play-once/chain INI with `fixtures/ini/kini.py`; kdh1 with `fixtures/dirhash/dirhash.py`; KAP handshake
  vector);
- `native/core/abi/kiln_api.h` (`KILN_PATH_CONFIG/STATE`, `KilnBootInfo` v3), `kiln_native.h`,
  `services/kiln_profile.h`;
- `crates/kiln-manage/src/lib.rs` (modules);
- `launcher/src-tauri/src/commands/` and `launcher/src/api/commands.ts` (command names).

Code:
| Path | What |
|---|---|
| `crates/kiln-manage` | the shared Rust library (kilnctl + launcher) |
| `runner/kilnctl` | the Linux server runner (CLI) |
| `launcher/` | the Kiln Launcher: React 18 + TypeScript + Vite (bun), Tauri 2 backend in `src-tauri/` |
| `mods/Kiln.Admin` | the admin op table (file bridge + KAP/1) |

Checks:
```sh
cargo test --workspace                                      # kiln-manage contract tests, kilnctl
cargo xwin check -p kiln-manage --target x86_64-pc-windows-msvc --all-features
dotnet test managed/tests/Kiln.Host.Tests                    # includes FixtureTests (C# side of parity)
python3 schema/fixtures/ini/kini.py check && python3 schema/fixtures/dirhash/dirhash.py --check
cd launcher && bun run check:schemas && bunx tsc --noEmit && bun run build
```
