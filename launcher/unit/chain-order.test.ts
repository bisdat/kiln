// dev.29: a compat rule that orders two DLLs (action `before`). The chain card says, under both entries, why their late
// switches are the rule's: Kiln's shipped rule for Auto Loot Plus and Ultimate ASI Loader in the player's words, a
// player's own rule without a message from the file names, and a rule that could not order them as a warning.
import { expect, test } from "bun:test";
import type { ChainViewEntry, CompatRuleResult } from "../src/api/commands";
import { entryOrder, orderSentence, orderSides, orderWarnings } from "../src/pages/profiles/chainOrder";

const UAL = "..\\xinput9_1_0.dll";
const ALP = "Z:\\home\\player\\.local\\share\\kiln\\library\\native\\auto-loot-plus\\0.0.0+5a9a5282\\dinput8.dll";
const SAID = "Ultimate ASI Loader loads after Kiln so that Auto Loot Plus can hook the game first; Auto Loot Plus's F8 menu needs that.";
const shipped: CompatRuleResult = {
  id: "auto-loot-plus-before-ultimate-asi-loader", source: "kiln", action: "before", message: SAID, fired: true, applied: true,
  targets: [UAL], early: [ALP],
};
const entry = (path: string, over: Partial<ChainViewEntry>): ChainViewEntry => ({
  path, file: path.split("\\").pop()!, section: "chain", engine: null, modId: null, late: false, lateBy: null, offBy: null, loader: false, ...over,
});

test("both switches the rule decides say so, with the rule's sentence", () => {
  const ual = entryOrder(entry(UAL, { engine: "ultimate-asi-loader", late: true, lateBy: `rule:${shipped.id}` }), [shipped]);
  expect(ual).toEqual({ rule: shipped, first: false, text: SAID, hint: `Loads late: rule ${shipped.id}` });
  // the profile's own switch said late: the rule keeps it first, and the switch is the rule's
  const alp = entryOrder(entry(ALP, { section: "profile", modId: "auto-loot-plus", earlyBy: `rule:${shipped.id}` }), [shipped]);
  expect(alp).toEqual({ rule: shipped, first: true, text: SAID, hint: `Loads first: rule ${shipped.id}` });
  expect(orderSides(shipped)).toBe("dinput8.dll before xinput9_1_0.dll");
});

test("entries no `before` rule decides keep their own switch", () => {
  const late: CompatRuleResult = { ...shipped, id: "xhl-after-sf", action: "late", early: undefined };
  expect(entryOrder(entry("chain\\xhl-core.dll", { late: true, lateBy: "rule:xhl-after-sf" }), [late])).toBeNull();
  expect(entryOrder(entry("chain\\xhl-core.dll", { late: true, lateBy: "player" }), [shipped])).toBeNull();
  // an older backend (no earlyBy), or a rule that was not applied
  expect(entryOrder(entry(ALP, { section: "profile" }), [shipped])).toBeNull();
  expect(entryOrder(entry(UAL, { lateBy: `rule:${shipped.id}` }), [{ ...shipped, applied: false }])).toBeNull();
  expect(orderSides(late)).toBeNull();
});

test("a rule without a message names the files", () => {
  expect(orderSentence({ ...shipped, id: "mine", message: "  ", source: "user" }))
    .toBe("xinput9_1_0.dll loads after Kiln and ShroudForge so that dinput8.dll loads first (compat rule mine).");
});

test("an order that cannot be made is a warning, nothing else", () => {
  const stuck: CompatRuleResult = {
    ...shipped, applied: false, targets: [], early: [],
    warning: "emf-dinput8.dll is EMF's loader: it starts EMF itself, so it always loads first and dinput8.dll cannot load before it. Nothing was moved.",
  };
  expect(orderWarnings([stuck, shipped, { ...stuck, id: "idle", fired: false }])).toEqual([{ id: shipped.id, text: stuck.warning! }]);
  expect(entryOrder(entry(UAL, { lateBy: `rule:${shipped.id}` }), [stuck])).toBeNull();
});
