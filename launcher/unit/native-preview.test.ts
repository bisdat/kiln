// Selected-game preview caching and display scope; all versions and manifests here are synthetic.
import { expect, test } from "bun:test";
import { QueryClient } from "@tanstack/react-query";
import { invalidateGame, qk } from "../src/api/hooks";
import type { ResolvePreview } from "../src/api/commands";
import { emptyMods } from "../src/api/kinds";
import { buildRows } from "../src/shell/mods";
import { steps } from "../src/pages/mods/LoadOrder";

const preview = (enabled: boolean): ResolvePreview => ({
  side: "client", kiln: { order: ["managed"], rejected: [] }, warnings: [], withWorld: [],
  native: [{ id: "native", version: "1.0.0", phase: "early", enabled,
    reason: enabled ? null : "requires kiln >=0.2.0-dev.31.1 (have 0.2.0-dev.30.9)" }],
});

test("the same profile and side have isolated preview caches for each game and for no game", () => {
  const qc = new QueryClient();
  try {
    qc.setQueryData(qk.resolve("p", "client", "a"), preview(true));
    qc.setQueryData(qk.resolve("p", "client", "b"), preview(false));
    qc.setQueryData(qk.resolve("p", "client"), preview(false));
    expect(qc.getQueryData(qk.resolve("p", "client", "a"))).toEqual(preview(true));
    expect(qc.getQueryData(qk.resolve("p", "client", "b"))).toEqual(preview(false));
    expect(qc.getQueryData(qk.resolve("p", "client", null))).toEqual(preview(false));
    expect(qc.getQueryData(qk.resolve("p", "server", "a"))).toBeUndefined();
  } finally { qc.clear(); }
});

test("install/rollback status invalidates all profile and side previews for only that game", () => {
  const qc = new QueryClient();
  const changed = [qk.resolve("p", "client", "a"), qk.resolve("p", "server", "a"), qk.resolve("other", "client", "a")];
  const untouched = [qk.resolve("p", "client", "b"), qk.resolve("p", "client", null)];
  try {
    for (const key of [...changed, ...untouched]) qc.setQueryData(key, preview(true));
    invalidateGame(qc, "a");
    for (const key of changed) expect(qc.getQueryState(key)?.isInvalidated).toBe(true);
    for (const key of untouched) expect(qc.getQueryState(key)?.isInvalidated).toBe(false);
  } finally { qc.clear(); }
});

test("native rejection is shown without changing the managed preview or optimistic no-preview rows", () => {
  const hash = "kdh1:" + "0".repeat(64);
  const input = {
    library: (["native", "kiln"] as const).map((kind) => ({ kind, id: kind === "kiln" ? "managed" : "native", name: kind,
      versions: [{ version: "1.0.0", hash, folder: "synthetic", imported: "", side: "both" as const, clients: "mod" as const,
        size: 1, source: null, manifestId: null, manifest: null }] })),
    profile: { format: 1, id: "p", name: "P", created: "", updated: "", generation: 1, notes: "", dropins: [], dataDir: "shared", server: null,
      mods: { ...emptyMods(), native: [{ id: "native", version: "1.0.0", hash, enabled: true }], kiln: [{ id: "managed", version: "1.0.0", hash, enabled: true }] } },
    found: [], launch: null, retained: [], xhlHost: null,
  };
  const rows = buildRows({ ...input, preview: preview(false) });
  expect(rows.find((r) => r.id === "native")?.wontRun).toContain("have 0.2.0-dev.30.9");
  expect(rows.find((r) => r.id === "managed")?.wontRun).toBeNull();
  expect(steps(rows, preview(false), "client").notLoading.map((r) => r.id)).toEqual(["native"]);
  const unknown = buildRows({ ...input, preview: null });
  expect(unknown.every((r) => !r.wontRun)).toBe(true); // existing optimistic display is not an installed-runtime verdict
  expect(steps(unknown, null, "client").steps).toEqual([]); // load order invents no resolver result
});
