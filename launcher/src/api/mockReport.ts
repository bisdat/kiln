// The browser mock's support report contents (report_preview, community_report_prepare): the files a real report of a
// Linux client with Building Tools and ShroudLink holds, with the replacements kiln_manage::report::contents counts.
import type { ReportContents, ReportContentsFile } from "./commands";

const FILES: Omit<ReportContentsFile, "excluded">[] = [
  { path: "summary.txt", group: "kiln", about: "summary", size: 6_210, excludable: false, replaced: { home: 4, user: 1 } },
  { path: "report.json", group: "kiln", about: "findings", size: 1_904, excludable: false, replaced: {} },
  { path: "game/kiln/logs/kiln.log", group: "kiln", about: "kiln-log", size: 22_408, excludable: false, replaced: { home: 169, player: 3, steamid: 2 } },
  { path: "game/kiln/logs/proxy.log", group: "kiln", about: "proxy-log", size: 3_115, excludable: false, replaced: { home: 6 } },
  { path: "game/kiln/crash-reports/2026-11-18T22-10-04Z-night-sky.txt", group: "kiln", about: "crash", size: 4_211, excludable: false, replaced: { home: 2 } },
  { path: "launcher/launcher.log", group: "kiln", about: "launcher-log", size: 18_090, excludable: false, replaced: { home: 41, ip: 1 } },
  { path: "game/BuildingTools.log", group: "mods", about: "building-tools", size: 9_870, excludable: true, replaced: { home: 12, player: 1 } },
  { path: "game/shroudlink.log", group: "mods", about: "shroudlink", size: 14_302, excludable: true, replaced: { player: 5, ip: 2, redacted: 1 } },
  { path: "game/kiln/state/boot.json", group: "settings", about: "state", size: 2_877, excludable: false, replaced: { home: 3 } },
  { path: "game/kiln/profile.json", group: "settings", about: "profile", size: 1_442, excludable: false, replaced: { home: 12 } },
  { path: "game/kiln/chain.ini", group: "settings", about: "chain", size: 211, excludable: false, replaced: {} },
  { path: "listings/game-folder.txt", group: "listings", about: "game-folder", size: 2_204, excludable: false, replaced: {} },
  { path: "listings/mod-folders.txt", group: "listings", about: "mod-folders", size: 3_980, excludable: false, replaced: {} },
];

const SUMMARY = `Kiln support report
Written 2026-11-20T09:00:00Z by kiln-launcher 0.2.0-dev (mock) (kiln-manage 0.2.0-dev).
Paths, user and machine names are replaced: see "Left out" at the end.

== Findings ==
- [problem] Night Sky 0.3.1 did not load: incompatible with Weather Plus 1.4.2.
- [warning] Kiln wrote a crash report on 2026-11-18 at 22:10 (1 in all; the newest is in this report).

== System ==
OS:            Arch Linux (kernel 6.18.2-arch1-1)
Steam Deck:    no

== Game ==
Folder:        <home>/.local/share/Steam/steamapps/common/Enshrouded
Side:          client
Steam build:   26430253

== Left out ==
Never collected:
- your saves and world data (the game's savegame folder, kiln/data/);
...
`;

export const NEVER: ReportContents["never"] = [
  { id: "saves", text: "your saves and world data (the game's savegame folder, kiln/data/)" },
  { id: "settings", text: "mod settings and Kiln's config folder (kiln/config/, the profile's config folder): they may hold server passwords and tokens" },
  { id: "admin", text: "Kiln.Admin's files (kiln/data/kiln-admin/: the admin token and endpoint)" },
  { id: "mods", text: "the contents of your mods (only folder and file names, sizes and times are listed, plus ShroudForge manifest ids and versions in findings)" },
  { id: "steam", text: "Steam's own files, besides the compat tool and launch options shown above" },
];

/** As kiln_manage::report::contents answers for a report built with `exclude` (only other mods' files can be left out). */
export function mockReportContents(exclude: unknown): ReportContents {
  const out = new Set(Array.isArray(exclude) ? exclude.map(String) : []);
  const gone = (f: Omit<ReportContentsFile, "excluded">) => f.excludable && (out.has(f.path) || out.has("other-mods"));
  const files = [...FILES.filter((f) => !gone(f)).map((f) => ({ ...f, excluded: false })), ...FILES.filter(gone).map((f) => ({ ...f, excluded: true }))];
  return { files, size: files.filter((f) => !f.excluded).reduce((n, f) => n + f.size, 0), summary: SUMMARY, never: NEVER };
}
