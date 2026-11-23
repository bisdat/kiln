// Pack Studio (src/studio/; DESIGN 3) in mock mode: a new pack from a profile and an empty one, the mods' choices,
// settings captured (never hand-written; a secret never offered), server game settings taken from a server, the load
// order with the pack layer last and a conflict set in the pack, the test instance and what the game reported,
// credits prefilled and completed, publish link-only then public (two clicks for an update), the server pack from a
// version, Save as file; Crucible off hides Studio; the Deck has no Studio. KILN_STUDIO_SHOTS=<dir> saves screenshots.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

test.use({ permissions: ["clipboard-read", "clipboard-write"] });
const SHOTS = process.env.KILN_STUDIO_SHOTS;
const shot = async (page: Page, name: string) => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: SHOTS + "/" + name + ".png", fullPage: false }); } };
type Req = { cmd: string; args: Record<string, unknown> };
const requests = (page: Page) => page.evaluate(() => (window as unknown as { __kilnStudioMock: { requests: () => Req[] } }).__kilnStudioMock.requests());
const state = (page: Page) => page.evaluate(() => (window as unknown as { __kilnStudioMock: { state: () => { source: Record<string, unknown>; credits: Record<string, unknown>; layer: Record<string, string> }[] } }).__kilnStudioMock.state());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test("a new pack starts from a profile, or empty with a profile of its own", async ({ page }) => {
  await open(page, "/?mock=1#/studio");
  await expect(page.getByTestId("studio-pack-ember-run")).toBeVisible();
  await shot(page, "desktop-studio-index");
  await page.getByTestId("studio-new").click();
  await page.getByTestId("studio-new-from").selectOption("vanilla-plus");
  await page.getByTestId("studio-create").click();
  await expect(page).toHaveURL(/#\/studio\/vanilla-plus\/mods$/);
  await expect(page.getByTestId("studio-mod-weather-plus")).toBeVisible();
  await page.goto("/?mock=1#/studio");
  await page.getByTestId("studio-new").click();
  await page.getByTestId("studio-new-name").fill("Frost Isles");
  await page.getByTestId("studio-create").click();
  await expect(page).toHaveURL(/#\/studio\/frost-isles\/mods$/);
  await expect(page.getByTestId("studio-mods-empty")).toBeVisible();
  const made = (await requests(page)).filter((r) => r.cmd === "studio_create").map((r) => r.args.request);
  expect(made).toEqual([{ name: "Vanilla plus", from: "vanilla-plus" }, { name: "Frost Isles", from: null }]);
});

test("mods: required or optional, which sides, and add from the Library", async ({ page }) => {
  await open(page, "/?mock=1#/studio/ember-run/mods");
  await expect(page.getByTestId("studio-mod-new-items")).toContainText("Kiln's own");
  await page.getByTestId("studio-dep-night-sky").selectOption("off");
  await page.getByTestId("studio-sides-night-sky").selectOption("client");
  await expect.poll(async () => (await state(page))[0].source.mods).toMatchObject({ "night-sky": { dep: "optional", default: false, env: { client: "required", server: "unsupported" } } });
  await shot(page, "desktop-studio-mods");
  await page.getByTestId("studio-add-library").click();
  await expect(page.getByTestId("studio-library")).toBeVisible();
  const first = page.getByTestId("studio-library").locator("input[type=checkbox]").first();
  await first.check();
  await page.getByTestId("studio-add-confirm").click();
  await expect(page.getByTestId("studio-library")).toHaveCount(0);
  await page.getByTestId("studio-remove-free-cam").click();
  await expect(page.getByTestId("studio-mod-free-cam")).toHaveCount(0);
});

test("settings are captured: chosen keys only, personal keys left out, secrets never offered", async ({ page }) => {
  await open(page, "/?mock=1#/studio/ember-run/settings");
  await expect(page.getByTestId("studio-key-big-stacks.json#multiplier")).toBeChecked();
  await page.getByTestId("studio-capture").click();
  // the capture reads the files again: the boxes take changes once it answered
  await expect(page.getByTestId("studio-capture")).toBeEnabled();
  await expect(page.getByTestId("studio-key-big-stacks.json#hotkey")).not.toBeChecked();
  await expect(page.getByTestId("studio-key-discord-bridge.json#token")).toBeDisabled();
  await expect(page.getByTestId("studio-whole-discord-bridge.json")).toBeDisabled();
  await page.getByTestId("studio-key-night-sky.json#stars.brightness").check();
  await page.getByTestId("studio-key-emf/beastmaster.ini#Pets.maxPets").check();
  await page.getByTestId("studio-whole-notes.txt").check();
  await shot(page, "desktop-studio-settings");
  await page.getByTestId("studio-capture-save").click();
  await expect.poll(async () => (await state(page))[0].source.overrides).toEqual([
    { path: "big-stacks.json", side: "both", keys: ["multiplier"] },
    { path: "night-sky.json", side: "both", keys: ["stars.brightness"] },
    { path: "emf/beastmaster.ini", side: "both", keys: ["Pets.maxPets"] },
    { path: "notes.txt", side: "both", keys: [] },
  ]);
  // game settings from a server: gameSettings only
  await page.getByTestId("studio-server-take").click();
  const boxes = page.getByTestId("studio-server-settings").locator("[data-testid^=studio-gs-]");
  await expect(boxes.first()).toBeVisible();
  await boxes.first().check();
  await page.getByTestId("studio-server-save").click();
  await expect(page.getByTestId("studio-server-current")).toBeVisible();
  const s = (await state(page))[0].source as { serverSettings: Record<string, unknown> };
  expect(Object.keys(s.serverSettings)).toEqual(["gameSettings"]);
});

test("load order: the pack layer last; a conflict is set in the pack", async ({ page }) => {
  await open(page, "/?mock=1#/studio/ember-run/order");
  const rows = page.getByTestId("studio-order").locator("ol li");
  await expect(page.getByTestId("studio-order-pack.ember-run")).toContainText("always last");
  const loads = await rows.evaluateAll((els) => els.filter((e) => !e.classList.contains("kst-off")).map((e) => (e as HTMLElement).dataset.testid));
  expect(loads.at(-1)).toBe("studio-order-pack.ember-run");
  const c = page.getByTestId("studio-conflict-items-maxStackSize");
  await expect(c).toContainText("big-stacks 1000");
  await shot(page, "desktop-studio-order");
  // the layer already decides that one; the other is decided here
  await expect(c.getByTestId("studio-conflict-set")).toHaveCount(0);
  const w = page.getByTestId("studio-conflict-items-weight");
  await expect(w).toContainText("weather-plus 0.5");
  await w.getByTestId("studio-conflict-value").fill("0.3");
  await w.getByTestId("studio-conflict-set").click();
  await expect(page.getByTestId("studio-conflict-items-weight")).toHaveCount(0);
  const sets = (await requests(page)).filter((r) => r.cmd === "studio_layer_set");
  expect(sets.map((r) => r.args)).toEqual([{ packId: "ember-run", section: "items", selector: "Material_T1_Twigs", field: "weight", value: 0.3 }]);
  expect((await state(page))[0].layer["content/pack.json"]).toContain('"weight": 0.3');
});

test("test this pack: a test instance, Play, and what the game reported", async ({ page }) => {
  await open(page, "/?mock=1#/studio/ember-run/test");
  await page.getByTestId("studio-test-make").click();
  await expect(page.getByTestId("studio-test-play")).toBeVisible({ timeout: 15_000 });
  await expect(page.getByTestId("studio-test-report")).toContainText("data conflict");
  await expect(page.getByTestId("studio-test-report")).toContainText("Storm_Ash");
  await shot(page, "desktop-studio-test");
  expect((await state(page))[0].source.testProfile).toBe("ember-run-test");
  // again: the loaded test instance goes back to the pack's profile first, then the new one is made (the game is passed)
  await page.getByTestId("studio-test-make").click();
  await expect.poll(async () => (await requests(page)).filter((r) => r.cmd === "studio_test").length).toBe(2);
  await expect(page.getByTestId("studio-test-make")).toBeEnabled({ timeout: 15_000 });
  await page.getByTestId("studio-test-end").click();
  await expect(page.getByTestId("studio-test-play")).toHaveCount(0);
  const end = (await requests(page)).filter((r) => r.cmd === "studio_test_end");
  expect(end[0].args.gameId).toBeTruthy();
});

test("credits: prefilled from the analysis, the rest filled in", async ({ page }) => {
  await open(page, "/?mock=1#/studio/ember-run/credits");
  await expect(page.getByTestId("studio-credit-new-items")).toContainText("Kiln's own");
  await expect(page.getByTestId("studio-credits-state")).toContainText("still need a credit");
  const rows = page.locator("[data-testid^=studio-credit-]");
  const ids = await rows.evaluateAll((els) => els.map((e) => (e as HTMLElement).dataset.testid!.replace("studio-credit-", "")));
  for (const id of ids) {
    const a = page.getByTestId("studio-author-" + id);
    if ((await a.inputValue()) === "") {
      await a.fill("someone");
      await page.getByTestId("studio-source-" + id).fill("https://www.nexusmods.com/enshrouded/mods/400");
    }
  }
  await page.getByTestId("studio-source-night-sky").fill("http://insecure");
  await expect(page.getByTestId("studio-source-night-sky")).toHaveAttribute("aria-invalid", "true");
  await page.getByTestId("studio-source-night-sky").fill("https://www.nexusmods.com/enshrouded/mods/313");
  await expect(page.getByTestId("studio-credits-state")).toContainText("Every mod is credited");
  await shot(page, "desktop-studio-credits");
  await page.getByTestId("studio-credits-save").click();
  await expect.poll(async () => Object.keys((await state(page))[0].credits).length).toBe(ids.length);
});

test("publish: signed out asks for the sign-in; link only, then public with credits; update in two clicks; server pack", async ({ page }) => {
  await open(page, "/?mock=1#/studio/ember-run/publish");
  await expect(page.getByTestId("studio-signin")).toBeVisible();
  await expect(page.getByTestId("studio-publish-go")).toBeDisabled();
  await open(page, "/?mock=1&discord=in#/studio/ember-run/publish");
  await expect(page.getByTestId("studio-checks")).toContainText("format");
  await page.getByTestId("studio-changes").fill("First version");
  await page.getByTestId("studio-publish-go").click();
  await expect(page.getByTestId("studio-published-link")).toContainText("frostdev.io/kiln/crucible", { timeout: 15_000 });
  await shot(page, "desktop-studio-published-link");
  await expect(page.getByTestId("studio-version")).toHaveValue("1.0.1");
  await expect(page.getByTestId("studio-version-1.0.0")).toBeVisible();
  // published: the typed notes went with it and are no unsaved work (which would hold the launcher update's restart)
  await expect(page.getByTestId("studio-changes")).toHaveValue("");
  await expect.poll(() => page.evaluate(async () => Object.values((await import("/src/shell/unsaved.ts")).useUnsaved.getState().by))).toEqual([]);
  // public: credits missing and no summary keep Publish off and say why
  await page.getByTestId("studio-vis-public").check();
  await expect(page.getByTestId("studio-needs")).toContainText("credits for");
  await expect(page.getByTestId("studio-publish-go")).toBeDisabled();
  await shot(page, "desktop-studio-public-needs");
  const pub = (await requests(page)).filter((r) => r.cmd === "studio_publish");
  expect(pub).toHaveLength(1);
  expect(pub[0].args.request).toMatchObject({ version: "1.0.0", changes: "First version", visibility: "link", attest: false });
  // the server pack from version 1.0.0
  await page.getByTestId("studio-server-pack-1.0.0").click();
  await page.getByTestId("studio-server-pack-go").click();
  await expect.poll(async () => (await requests(page)).some((r) => r.cmd === "studio_server_pack" && r.args.version === "1.0.0")).toBe(true);
});

test("a public pack's update is two clicks: Publish tab, Publish", async ({ page }) => {
  await open(page, "/?mock=1&discord=in&studio=published#/studio/ember-run/publish");
  await expect(page.getByTestId("studio-vis-public")).toBeChecked();
  await expect(page.getByTestId("studio-attest")).toBeChecked();
  await expect(page.getByTestId("studio-version")).toHaveValue("1.0.1");
  await expect(page.getByTestId("studio-publish-go")).toBeEnabled();
  await page.getByTestId("studio-publish-go").click();
  await expect(page.getByTestId("studio-published-link")).toContainText("frostdev.io/kiln/crucible/ember-run", { timeout: 15_000 });
  await shot(page, "desktop-studio-public-update");
  const pub = (await requests(page)).filter((r) => r.cmd === "studio_publish");
  expect(pub[0].args.request).toMatchObject({ version: "1.0.1", visibility: "public", attest: true });
});

test("Crucible not ready for public yet: the pack stays link-only with the reason, then Make public", async ({ page }) => {
  await open(page, "/?mock=1&discord=in&studio=notready#/studio/ember-run/credits");
  // complete the credits first
  const ids = await page.locator("[data-testid^=studio-credit-]").evaluateAll((els) => els.map((e) => (e as HTMLElement).dataset.testid!.replace("studio-credit-", "")));
  for (const id of ids) {
    if ((await page.getByTestId("studio-author-" + id).inputValue()) === "") {
      await page.getByTestId("studio-author-" + id).fill("someone");
      await page.getByTestId("studio-source-" + id).fill("https://www.nexusmods.com/enshrouded/mods/400");
    }
  }
  await page.getByTestId("studio-credits-save").click();
  await expect(page.getByTestId("studio-credits-state")).toContainText("Every mod is credited");
  await page.getByTestId("studio-tab-publish").click();
  await page.getByTestId("studio-vis-public").check();
  await page.getByTestId("studio-attest").check();
  await page.getByTestId("studio-publish-go").click();
  await expect(page.getByTestId("studio-vis-needs")).toContainText("checks are still running", { timeout: 15_000 });
  await shot(page, "desktop-studio-not-ready");
  await page.getByTestId("studio-set-visibility").click();
  await expect(page.getByTestId("studio-state")).toHaveText(/public/i);
  const sv = (await requests(page)).filter((r) => r.cmd === "studio_set_visibility");
  expect(sv.map((r) => r.args)).toEqual([{ packId: "ember-run", visibility: "public", attest: true }]);
});

test("a pack deleted on Crucible: publish says so and only Publish as a new pack goes on", async ({ page }) => {
  await open(page, "/?mock=1&discord=in&studio=published,gone#/studio/ember-run/publish");
  await page.getByTestId("studio-publish-go").click();
  await expect(page.getByTestId("studio-publish-go")).toHaveText("Publish as a new pack");
  await shot(page, "desktop-studio-gone");
  await page.getByTestId("studio-publish-go").click();
  await expect(page.getByTestId("studio-published-link")).toBeVisible({ timeout: 15_000 });
  const pub = (await requests(page)).filter((r) => r.cmd === "studio_publish");
  expect(pub.map((r) => (r.args.request as { fresh?: boolean }).fresh)).toEqual([false, true]);
});

test("Crucible off hides Studio", async ({ page }) => {
  await open(page, "/?mock=1&crucible=off#/");
  await expect(page.locator(".k-nav")).not.toContainText("Studio");
  await page.goto("/?mock=1&crucible=off#/studio");
  await expect(page).toHaveURL(/#\/$/);
  await open(page, "/?mock=1#/");
  await expect(page.locator(".k-nav")).toContainText("Studio");
});

test.describe("Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  test("the Deck has no Studio (Studio-made packs install like any other pack)", async ({ page }) => {
    await page.goto("/?deck=steamdeck#/");
    await expect(page.locator('[data-deck-screen="home"]')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator("body")).not.toContainText("Pack Studio");
    await expect(page.locator("body")).not.toContainText("Studio");
    await shot(page, "deck-home-no-studio");
  });
});
