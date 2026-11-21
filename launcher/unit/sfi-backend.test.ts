// Settings' ShroudForge card: its rows and when a choice may be written
// (src/components/shroudForgeBackend.ts).
import { expect, test } from "bun:test";
import type { SfBackendCard, SfBackendPreview, SfResolution } from "../src/api/commands";
import { isShroudForgePath } from "../src/components/ShroudForgeSettings";
import { SF_CHOICES, canSave, choiceRows, chosenOf, draftFor, type Draft } from "../src/components/shroudForgeBackend";
import snapshot from "../src/data/kiln-json.snapshot.json";
import en from "../src/i18n/en.json";

const HELD = "game test pending: sfi-backend-roundtrip";
const next = (selected: SfResolution["selected"]): SfResolution =>
  ({ requested: selected, selected, source: "setting", refusal: null, reason: "r", envOffNote: null });
const card = (o: Partial<SfBackendCard> = {}): SfBackendCard =>
  ({ gameId: "a", setting: { kind: "default" }, next: next("hosted"), revision: "r1", interpreterHeld: HELD, ...o });
const preview = (o: Partial<SfBackendPreview> = {}): SfBackendPreview =>
  ({ gameId: "a", backend: "installed", revision: "r1", next: next("installed"), unchanged: false, ...o });

test("three choices, hosted the default; ShroudForge 1.8 is never one", () => {
  expect([...SF_CHOICES]).toEqual(["hosted", "installed", "kiln"]);
  expect(JSON.stringify(SF_CHOICES)).not.toMatch(/sf18|1\.8/);
  const rows = choiceRows(card(), false);
  expect(rows.map((r) => [r.backend, r.checked, r.isDefault])).toEqual([["hosted", true, true], ["installed", false, false], ["kiln", false, false]]);
  // the card's words never offer 1.8 either
  expect(JSON.stringify((en as Record<string, unknown>)["sfBackend"])).not.toMatch(/sf18|1\.8/);
});

test("default, inherited and explicit mark their backend; an invalid value marks none", () => {
  expect(chosenOf(card())).toBe("hosted");
  expect(chosenOf(card({ setting: { kind: "inherited", backend: "installed" } }))).toBe("installed");
  expect(chosenOf(card({ setting: { kind: "explicit", backend: "kiln" } }))).toBe("kiln");
  expect(chosenOf(card({ setting: { kind: "invalid", value: "\"x\"" } }))).toBe(null);
  expect(choiceRows(card({ setting: { kind: "invalid", value: "1" } }), false).some((r) => r.checked)).toBe(false);
});

test("kiln is greyed with its reason while held, and can be asked for once lifted", () => {
  const held = choiceRows(card(), false).find((r) => r.backend === "kiln")!;
  expect(held).toMatchObject({ disabled: true, held: HELD });
  expect(draftFor(card(), "kiln", false)).toBe(null);
  const open = card({ interpreterHeld: undefined });
  expect(choiceRows(open, false).find((r) => r.backend === "kiln")).toMatchObject({ disabled: false });
  expect(draftFor(open, "kiln", false)).toEqual({ gameId: "a", backend: "kiln", revision: "r1" });
});

test("nothing is asked while the game runs, for kiln.json that cannot be read, or for the row already chosen", () => {
  expect(choiceRows(card(), true).every((r) => r.disabled)).toBe(true);
  expect(draftFor(card(), "installed", true)).toBe(null);
  expect(choiceRows(card({ unreadable: "does not parse" }), false).every((r) => r.disabled)).toBe(true);
  expect(draftFor(card({ unreadable: "x" }), "installed", false)).toBe(null);
  expect(draftFor(card(), "hosted", false)).toBe(null);
  expect(draftFor(card(), null, false)).toBe(null);
  // back to the default from a choice, from the older switch and from an invalid value
  for (const setting of [{ kind: "explicit", backend: "installed" }, { kind: "inherited", backend: "installed" }, { kind: "invalid", value: "1" }] as const)
    expect(draftFor(card({ setting }), null, false)).toEqual({ gameId: "a", backend: null, revision: "r1" });
});

test("Save writes only a preview of exactly this draft, for the game shown, on the bytes the card read", () => {
  const d: Draft = { gameId: "a", backend: "installed", revision: "r1" };
  expect(canSave(d, "a", card(), preview())).toBe(true);
  // a late answer for another game, or the card now showing another game
  expect(canSave(d, "b", card(), preview())).toBe(false);
  expect(canSave(d, "a", card(), preview({ gameId: "b" }))).toBe(false);
  expect(canSave(d, "a", card({ gameId: "b" }), preview())).toBe(false);
  expect(canSave({ ...d, gameId: "b" }, "a", card(), preview())).toBe(false);
  // another choice's answer, older bytes, a card read again since
  expect(canSave(d, "a", card(), preview({ backend: "kiln" }))).toBe(false);
  expect(canSave(d, "a", card(), preview({ revision: "r0" }))).toBe(false);
  expect(canSave(d, "a", card({ revision: "r2" }), preview())).toBe(false);
  // refused, nothing to write, no answer yet
  expect(canSave(d, "a", card(), preview({ refused: HELD, next: undefined }))).toBe(false);
  expect(canSave(d, "a", card(), preview({ unchanged: true }))).toBe(false);
  expect(canSave(d, "a", card(), undefined)).toBe(false);
  expect(canSave(null, "a", card(), preview())).toBe(false);
  expect(canSave(d, null, card(), preview())).toBe(false);
});

test("the kiln.json card leaves shroudforge.* to this card", () => {
  const paths = (snapshot as { entries: { path: string }[] }).entries.map((e) => e.path);
  expect(paths.filter(isShroudForgePath)).toEqual(["shroudforge.useInstalled"]);
  expect(isShroudForgePath("shroudforge")).toBe(true);
  expect(isShroudForgePath("shroudforgeX.a")).toBe(false);
});
