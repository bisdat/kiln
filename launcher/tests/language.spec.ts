// The launcher's languages (src/i18n, docs/kiln/translating.md) in mock mode, with the shipped Simplified Chinese
// (src/i18n/zh-CN.json, by XiaoHuLi666): chosen in Settings, Language on the desktop and with the Language row of the
// Deck's Settings, the page follows at once, <html lang> names it, no key shows in place of a text, dates follow it, and
// English comes back. `KILN_LANG_SHOTS=<dir>` also saves screenshots there (to look for boxes and cut labels).
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

function keysOf(o: Record<string, unknown>, prefix = "", out: string[] = []): string[] {
  for (const [k, v] of Object.entries(o)) {
    const key = prefix ? `${prefix}.${k}` : k;
    if (v !== null && typeof v === "object") keysOf(v as Record<string, unknown>, key, out);
    else out.push(key);
  }
  return out;
}
// keys with two dots or more: a word like "common.ok" never reads as a sentence, so seeing one means a missing text
const KEYS = keysOf(JSON.parse(readFileSync(join(import.meta.dirname, "..", "src", "i18n", "en.json"), "utf8"))).filter((k) => k.split(".").length > 2);

/** Keys shown as text on the page (a text neither the language nor English has). */
const keysShown = (page: Page) => page.evaluate((keys) => {
  const text = document.body.innerText;
  return keys.filter((k) => text.includes(k));
}, KEYS);

async function shot(page: Page, name: string) {
  const dir = process.env.KILN_LANG_SHOTS;
  if (!dir) return;
  mkdirSync(dir, { recursive: true });
  await page.waitForTimeout(1200);  // page transitions settle
  await page.screenshot({ path: `${dir}/${name}.png` });
}

const lang = (page: Page) => page.evaluate(() => document.documentElement.lang);

test.describe("desktop", () => {
  test("Settings, Language lists 简体中文; choosing it shows the launcher in Chinese at once, and English comes back", async ({ page }) => {
    await page.goto("/#/settings");
    await page.locator(".k-splash").waitFor({ state: "detached" });
    const select = page.locator("#st-lang");
    await expect(select).toBeEnabled();
    await expect(select.locator("option")).toContainText(["System (default)", "English", "简体中文"]);
    await select.selectOption("zh-CN");
    await expect.poll(() => lang(page)).toBe("zh-CN");
    await expect(page.locator(".k-page-title")).toHaveText("设置");
    await expect(page.getByRole("navigation").first()).toContainText("开始游戏");
    await expect(page).toHaveTitle("设置 · Kiln");
    await expect(page.locator("#st-lang option:checked")).toHaveText("简体中文");
    expect(await keysShown(page)).toEqual([]);
    await shot(page, "desktop-settings-zh-CN");
    // a date in Chinese: the Saves page's last change (mock saves are from early November: shown as a date)
    await page.evaluate(() => { location.hash = "#/saves"; });
    await expect(page.locator(".k-page-title")).toHaveText("存档");
    await expect(page.getByText(/^2026\/11\/\d{1,2}$/).first()).toBeVisible();
    expect(await keysShown(page)).toEqual([]);
    await shot(page, "desktop-saves-zh-CN");
    await page.evaluate(() => { location.hash = "#/"; });
    await expect.poll(() => keysShown(page)).toEqual([]);
    await shot(page, "desktop-play-zh-CN");
    // and back
    await page.evaluate(() => { location.hash = "#/settings"; });
    await page.locator("#st-lang").selectOption("en");
    await expect.poll(() => lang(page)).toBe("en");
    await expect(page.locator(".k-page-title")).toHaveText("Settings");
  });

  test("System follows a Chinese system language (zh-Hans-CN, as browsers report it)", async ({ browser }) => {
    const context = await browser.newContext({ locale: "zh-CN" });
    const page = await context.newPage();
    await page.addInitScript(() => Object.defineProperty(navigator, "languages", { get: () => ["zh-Hans-CN", "zh-CN", "en"] }));
    await page.goto("/#/settings");
    await page.locator(".k-splash").waitFor({ state: "detached" });
    await expect(page.locator("#st-lang")).toHaveValue("system");
    await expect.poll(() => lang(page)).toBe("zh-CN");
    await expect(page.locator(".k-page-title")).toHaveText("设置");
    await context.close();
  });
});

test.describe("Steam Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });

  /** A press and release on the native reader's event (src-tauri/src/deck/gamepad.rs). */
  async function pad(page: Page, button: string) {
    for (const pressed of [true, false])
      await page.evaluate(([b, p]) => (window as unknown as { __kilnMock: { emit: (n: string, v: unknown) => void } }).__kilnMock
        .emit("kiln://gamepad", { kind: "button", button: b, pressed: p, repeat: false, device: "event9" }), [button, pressed] as const);
    await page.waitForTimeout(60);
  }

  test("the Language row steps through System, English and 简体中文; the Deck follows at once", async ({ page }) => {
    await page.goto("/?deck=steamdeck#/deck/settings");
    const row = page.locator('[data-deck-id="set-language"]');
    await expect(row).toBeEnabled();
    await expect(row).toContainText("Language");
    await expect(row).toContainText("System (default)");
    await row.focus();
    await pad(page, "a");
    await expect(row).toContainText("English");
    await pad(page, "a");
    await expect(row).toContainText("简体中文");
    await expect(row).toContainText("语言");
    await expect.poll(() => lang(page)).toBe("zh-CN");
    await expect(page.locator(".deck-tab").filter({ hasText: "设置" })).toHaveCount(1);
    await expect(page.locator('[data-deck-id="set-glyphs"]')).toContainText("按键图标");
    expect(await keysShown(page)).toEqual([]);
    await shot(page, "deck-settings-zh-CN");
    await page.evaluate(() => { location.hash = "#/deck"; });  // another screen: still Chinese
    await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
    await expect.poll(() => lang(page)).toBe("zh-CN");
    await expect.poll(() => keysShown(page)).toEqual([]);
    await shot(page, "deck-home-zh-CN");
    await page.evaluate(() => { location.hash = "#/deck/settings"; });
    const again = page.locator('[data-deck-id="set-language"]');
    await expect(again).toContainText("简体中文");
    await again.focus();
    await pad(page, "a");  // after the last language: System again (English in this browser)
    await expect(again).toContainText("System (default)");
    await expect.poll(() => lang(page)).toBe("en");
    await expect(again).toContainText("Language");
  });
});
