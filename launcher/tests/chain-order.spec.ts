// dev.29: a compat rule that orders two DLLs (action `before`; kiln_manage::compat). Kiln's shipped rule loads Ultimate
// ASI Loader late and keeps Auto Loot Plus first, whatever their "Load after Kiln and ShroudForge" switches say. The
// chain card (Native and DLLs, /mods/native) shows both switches as the rule's, says why under each, and offers no switch
// against it; a rule that cannot order its DLLs is a warning. Browser mock, the view injected (src/api/mock.ts plans).
import { expect, test, type Page } from "@playwright/test";
import type { ChainView, ChainViewEntry, CompatRuleResult } from "../src/api/commands";

async function nativePage(page: Page) {
  await page.goto("/#/mods/native");
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
const card = (page: Page) => page.getByRole("region", { name: "Closed DLL mods" });
const entries = (page: Page) => page.getByRole("list", { name: "Chain entries" });
const row = (page: Page, file: string) => entries(page).getByRole("listitem").filter({ hasText: file });
const toggle = (page: Page, file: string) => row(page, file).getByRole("switch", { name: "Load after Kiln and ShroudForge" });

const RULE = "auto-loot-plus-before-ultimate-asi-loader";
const SAID = "Ultimate ASI Loader loads after Kiln so that Auto Loot Plus can hook the game first; Auto Loot Plus's F8 menu needs that.";
const UAL = "..\\xinput9_1_0.dll";
const ALP = "Z:\\home\\player\\.local\\share\\kiln\\library\\native\\auto-loot-plus\\0.0.0+5a9a5282\\dinput8.dll";
const ual: ChainViewEntry = {
  path: UAL, file: "xinput9_1_0.dll", section: "chain", engine: "ultimate-asi-loader", modId: null, late: true, lateBy: `rule:${RULE}`,
  offBy: null, earlyBy: null, loader: false, gated: false,
};
// the profile's own switch says late (as in the player's report); the rule keeps it first
const alp: ChainViewEntry = {
  path: ALP, file: "dinput8.dll", section: "profile", engine: null, modId: "auto-loot-plus", late: false, lateBy: null,
  offBy: null, earlyBy: `rule:${RULE}`, loader: false, gated: false,
};
const rule: CompatRuleResult = { id: RULE, source: "kiln", action: "before", message: SAID, fired: true, applied: true, targets: [UAL], early: [ALP] };
const planView = (page: Page, view: ChainView) => page.addInitScript((result) => {
  (window as unknown as { __kilnMockPlans: unknown }).__kilnMockPlans = { chain_view: Array.from({ length: 40 }, () => ({ result })) };
}, view);
const viewOf = (e: ChainViewEntry[], rules: CompatRuleResult[]): ChainView =>
  ({ entries: e, rules, ruleErrors: [], session: null, overlaps: [], active: true, current: true, error: null });

test("both switches a load-order rule decides show it, say why, and are not offered", async ({ page }) => {
  await planView(page, viewOf([ual, alp], [rule]));
  await nativePage(page);
  for (const f of ["xinput9_1_0.dll", "dinput8.dll"]) {
    await expect(row(page, f)).toContainText(SAID);
    await expect(toggle(page, f)).toBeDisabled();
  }
  await expect(toggle(page, "xinput9_1_0.dll")).toBeChecked();
  await expect(toggle(page, "dinput8.dll")).not.toBeChecked();
  await expect(row(page, "xinput9_1_0.dll")).toContainText(`Loads late: rule ${RULE}`);
  await expect(row(page, "dinput8.dll")).toContainText(`Loads first: rule ${RULE}`);
  const rules = page.getByRole("list", { name: "Active rules" });
  await expect(rules).toContainText("Sets the load order");
  await expect(rules).toContainText("dinput8.dll before xinput9_1_0.dll");
  await expect(card(page).getByText("Load order not changed")).toHaveCount(0);
});

test("a rule that cannot order its DLLs is a warning, and the switches stay the player's", async ({ page }) => {
  const warning = "xinput9_1_0.dll is shroudlink's client addon: Kiln's start waits for it, so it always loads first and dinput8.dll cannot load before it. Nothing was moved.";
  const stuck: CompatRuleResult = { ...rule, applied: false, targets: [], early: [], warning };
  await planView(page, viewOf([{ ...ual, late: false, lateBy: null }, { ...alp, earlyBy: null, late: true, lateBy: "profile" }], [stuck]));
  await nativePage(page);
  await expect(card(page).getByText("Load order not changed")).toBeVisible();
  await expect(card(page)).toContainText(warning);
  await expect(toggle(page, "dinput8.dll")).toBeEnabled();
  await expect(toggle(page, "xinput9_1_0.dll")).toBeEnabled();
  await expect(row(page, "dinput8.dll")).not.toContainText(SAID);
});
