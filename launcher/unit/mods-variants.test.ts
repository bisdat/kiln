// One download can hold a game copy and a dedicated-server copy of one mod with one version (Town Portals 2.0.3: its zip
// has client/ and server/, inbox 91). The Library keeps both and says which side the download packed each for
// (LibraryVersion.source.packagedFor); what the launcher picks by itself (src/shell/mods.ts defaultVersion) is the game
// copy for a game's profile and the server copy in the server's sync dialog, never whichever the Library lists first.
import { expect, test } from "bun:test";
import type { LibraryEntry, LibraryVersion, Profile } from "../src/api/commands";
import { emptyMods } from "../src/api/kinds";
import { buildRows, defaultVersion, type ModInputs } from "../src/shell/mods";

const hash = (s: string) => `kdh1:${s.padEnd(64, "0").slice(0, 64)}`;
const copy = (v: string, h: string, packagedFor?: "client" | "server"): LibraryVersion => ({
  version: v, hash: hash(h), folder: `library/greeneye/town-portal/${v}+${h}`, imported: "2026-11-19T00:00:00Z", side: "both", clients: "none",
  size: 1000, source: { type: "file", from: "Town Portals-255-2-0-3.zip", ...(packagedFor ? { packagedFor } : {}) }, manifestId: null, manifest: null,
});
const lib = (versions: LibraryVersion[]): LibraryEntry => ({ kind: "greeneye", id: "town-portal", name: "Town Portals", versions });
const profile: Profile = { format: 1, id: "p", name: "P", created: "", updated: "", generation: 1, notes: "", mods: emptyMods(), dropins: [], dataDir: "shared", server: null };
const inputs = (x: Partial<ModInputs>): ModInputs => ({ library: [], profile: null, preview: null, found: [], launch: null, retained: [], xhlHost: null, ...x });

test("a game gets the client copy and a server the server copy, in either Library order", () => {
  for (const versions of [[copy("2.0.3", "s", "server"), copy("2.0.3", "c", "client")], [copy("2.0.3", "c", "client"), copy("2.0.3", "s", "server")]]) {
    expect(defaultVersion(lib(versions))?.hash).toBe(hash("c"));
    expect(defaultVersion(lib(versions), "server")?.hash).toBe(hash("s"));
  }
  // a copy with no word beats one packed for the other side; a newer version still wins over any copy of an older one
  expect(defaultVersion(lib([copy("2.0.3", "s", "server"), copy("2.0.3", "n")]))?.hash).toBe(hash("n"));
  expect(defaultVersion(lib([copy("2.1.0", "s", "server"), copy("2.0.3", "c", "client")]))?.version).toBe("2.1.0");
});

test("a mod the profile lacks shows and switches on the client copy", () => {
  const [row] = buildRows(inputs({ library: [lib([copy("2.0.3", "s", "server"), copy("2.0.3", "c", "client")])], profile }));
  expect(row.version).toBe("2.0.3");
  expect(defaultVersion(row.lib)?.hash).toBe(hash("c"));
});
