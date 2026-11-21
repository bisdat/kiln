// the ShroudForge row of the Engines view in kiln mode and the restore's BASE: the phase word
// and its tone, the reason, a stale or unreadable record as unknown (never running or done), one line per package with
// its tone, the start before it apart; the BASE by its exact label, never vanilla, with Steam's Verify for vanilla; the
// strings in all 11 locales.
import { expect, test } from "bun:test";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import type { DataRestoreBase, EngineStatus, Json } from "../src/api/commands";
import { isSfiRow, sfiView, SfiEngineRow } from "../src/components/SfiEngineRow";
import { SfiRestoreBase } from "../src/components/RestoreDataOffer";
import en from "../src/i18n/en.json";
import de from "../src/i18n/de.json";
import es from "../src/i18n/es.json";
import fr from "../src/i18n/fr.json";
import it from "../src/i18n/it.json";
import ja from "../src/i18n/ja.json";
import ko from "../src/i18n/ko.json";
import pl from "../src/i18n/pl.json";
import ptBR from "../src/i18n/pt-BR.json";
import ru from "../src/i18n/ru.json";
import zhCN from "../src/i18n/zh-CN.json";

const row = (state: Json | null, error?: string): EngineStatus => ({ kind: "shroudforge", plugin: "kiln-sfi", state, file: "/g/kiln/state/sfi.json", error: error ?? null });
const FIRST = "unclassified: game files as found when Kiln first managed this install";

test("only kiln-sfi's ShroudForge row is the interpreter's", () => {
  expect(isSfiRow(row(null))).toBe(true);
  expect(isSfiRow({ ...row(null), plugin: "kiln-shroudforge" })).toBe(false);
  expect(isSfiRow({ ...row(null), kind: "eml" })).toBe(false);
});

test("the phase word with its tone, the reason and one line per package", () => {
  const v = sfiView(row({ mode: "kiln interpreter", status: "running", previous: "incomplete", mods: [
    { id: "a", version: "1.0.0", state: "running" }, { id: "b", version: "1.0.0", state: "data applied" },
    { id: "c", version: "0.7.2", state: "skipped", reason: "needs runtime.ui.draw_list" }, { id: "d", version: "1.0.0", state: "off" },
    { id: "e", version: null, state: "loading" }] }));
  expect([v.word, v.tone, v.reason]).toEqual(["running", "ok", null]);
  expect(v.previous).toContain("ended without finishing its record");
  expect(v.lines.map((l) => [l.id, l.word, l.tone])).toEqual([["a", "running", "ok"], ["b", "data applied", "ok"], ["c", "skipped", "warn"], ["d", "off", "idle"], ["e", "loading", "info"]]);
  expect(v.lines[2].reason).toBe("needs runtime.ui.draw_list");
  const refused = sfiView(row({ status: "refused", reason: "kiln-eml's startup pass granted no archive generation", mods: [] }));
  expect([refused.word, refused.tone, refused.reason]).toEqual(["not started", "warn", "kiln-eml's startup pass granted no archive generation"]);
  expect(sfiView(row({ status: "ended", mods: [] })).tone).toBe("idle");
  expect(sfiView(row({ status: "aside", reason: "hosted", mods: [] })).word).toBe("stood aside");
  // a phase this launcher does not know: as written, never ok
  expect(sfiView(row({ status: "paused", mods: [] }))).toMatchObject({ word: "paused", tone: "info" });
});

test("a record that is not this start's and a file Kiln cannot read are unknown, never running or done", () => {
  const stale = sfiView(row({ status: "unknown", recordPhase: "running", staleCode: "not-running", reason: "backend text", mods: [] }));
  expect([stale.word, stale.tone]).toEqual(["unknown", "warn"]);
  expect(stale.reason).toBe("The record says running, but the game is not running: that start ended without its last record (it may have crashed).");
  expect(sfiView(row({ status: "unknown", staleCode: "older-start", mods: [] })).reason).toContain("earlier start");
  // a code this launcher does not know: the backend's own text
  expect(sfiView(row({ status: "unknown", staleCode: "newer-rule", reason: "backend text", mods: [] })).reason).toBe("backend text");
  const broken = sfiView(row(null, "/g/kiln/state/sfi.json: not kiln-sfi's state (format 1, writer kiln-sfi)"));
  expect([broken.word, broken.tone, broken.lines.length]).toEqual(["unknown", "warn", 0]);
  expect(broken.reason).toContain("not kiln-sfi's state");
  const html = renderToStaticMarkup(<SfiEngineRow s={row({ status: "unknown", recordPhase: "running", staleCode: "not-running", mods: [] })} />);
  expect(html).toContain('data-sfi-phase="unknown"');
  expect(html).toContain("k-tone-warn");
  expect(html).not.toContain("k-tone-ok");
});

function base(b: DataRestoreBase | null) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  client.setQueryData(["eml_data_base", "test"], b);
  return renderToStaticMarkup(<QueryClientProvider client={client}><SfiRestoreBase gameId="test" /></QueryClientProvider>);
}

test("the restore's BASE: its exact label, how to get there, Steam's Verify for vanilla; unknown says why", () => {
  expect(base(null)).toBe("");
  const html = base({ state: "recorded", token: "first-managed-unclassified", label: FIRST, baselineSha256: "ab".repeat(32), reason: null });
  expect(html).toContain(`Kiln started from these game files: ${FIRST}.`);
  expect(html).toContain('data-base-origin="first-managed-unclassified"');
  expect(html).toContain(`data-baseline="${"ab".repeat(32)}"`);
  expect(html).toContain("Verify integrity of game files");
  expect(html).toContain("do not save a world while they are off");
  expect(html.toLowerCase()).not.toContain("vanilla");
  const unknown = base({ state: "unknown", token: null, label: null, baselineSha256: null, reason: "the backups are gone" });
  expect(unknown).toContain("Kiln cannot tell which game files it would put back: the backups are gone.");
  expect(unknown).toContain('data-base-origin="unknown"');
  expect(unknown).not.toContain("Kiln started from");
});

type Tree = { [k: string]: string | Tree };
const keys = (o: Tree, pre = ""): string[] => Object.entries(o).flatMap(([k, v]) => typeof v === "string" ? [pre + k] : keys(v, `${pre}${k}.`));
const sfi = (l: unknown) => (l as { "sfBackend": Tree })["sfBackend"];
test("every locale has the row's and the BASE's strings, the BASE titled as the restore banner", () => {
  const want = [...keys(sfi(en).engineRow as Tree, "engineRow."), ...keys(sfi(en).restoreBase as Tree, "restoreBase.")];
  for (const l of [en, de, es, fr, it, ja, ko, pl, ptBR, ru, zhCN]) {
    const got = [...keys(sfi(l).engineRow as Tree, "engineRow."), ...keys(sfi(l).restoreBase as Tree, "restoreBase.")];
    expect(got.sort()).toEqual([...want].sort());
    expect((sfi(l).restoreBase as Tree).title).toBe((l as { restoreDataOffer: { title: string } }).restoreDataOffer.title);
    expect((sfi(l).restoreBase as Tree).base as string).toContain("{label}");
  }
});
