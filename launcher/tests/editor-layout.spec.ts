// Hearth presentation regressions use the mounted editor and synthetic saves only. Measure the boxes that used to
// clip, including their scroll containers: text presence alone does not prove a control can be seen or reached.
import { expect, test, type Locator, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const LOCALES = ["en", "de", "es", "fr", "it", "ja", "ko", "pl", "pt-BR", "ru", "zh-CN"];
const SIZES = [{ width: 1024, height: 640 }, { width: 1280, height: 720 }, { width: 1920, height: 1080 }];
async function open(page: Page, deck = false, locale = "en") {
  await page.goto(deck ? "/?deck=steamdeck#/deck/editor" : "/#/editor");
  await expect(page.locator(".ke-grid .ke-slot.full").first()).toBeVisible({ timeout: 15000 });
  await page.evaluate(async (tag) => { (await import("/src/i18n/index.ts")).setLanguage(tag); }, locale);
  await page.evaluate(() => document.fonts.ready);
  // The shell's entrance motion otherwise moves the measured boxes between samples.
  await page.waitForTimeout(750);
}
async function shot(page: Page, name: string) {
  if (!process.env.KILN_EDITOR_LAYOUT_SHOTS) return;
  mkdirSync(process.env.KILN_EDITOR_LAYOUT_SHOTS, { recursive: true });
  await page.screenshot({ path: join(process.env.KILN_EDITOR_LAYOUT_SHOTS, `${name}.png`) });
}
async function contained(child: Locator, parent?: Locator, vertical = true, ring = 0) {
  const box = await child.boundingBox();
  expect(box, `visible box for ${child}`).not.toBeNull();
  const bounds = parent ? await parent.boundingBox() : await child.page().evaluate(() => ({ x: 0, y: 0, width: innerWidth, height: innerHeight }));
  expect(bounds).not.toBeNull();
  expect(box!.x - ring, `${child} left`).toBeGreaterThanOrEqual(bounds!.x - 1);
  expect(box!.x + box!.width + ring, `${child} right`).toBeLessThanOrEqual(bounds!.x + bounds!.width + 1);
  if (vertical) {
    expect(box!.y - ring, `${child} top`).toBeGreaterThanOrEqual(bounds!.y - 1);
    expect(box!.y + box!.height + ring, `${child} bottom`).toBeLessThanOrEqual(bounds!.y + bounds!.height + 1);
  }
}
async function inventory(page: Page, deck = false) {
  const split = page.locator(".ke-split"), main = split.locator(".ke-main"), insp = split.locator(".ke-insp");
  await contained(split);
  const stacked = !deck && page.viewportSize()!.width === 1024;
  // At the minimum height the stacked view scrolls vertically. Check its full width, then reveal the inspector's
  // actual controls and check them against both the scrolling well and the viewport.
  await contained(main, split, !stacked);
  await contained(insp, split, !stacked);
  for (const grid of await main.locator(".ke-grid").all()) {
    await contained(grid, main, false);
    await contained(grid.locator(".ke-cell").nth(7), main, false);
  }
  for (const control of await insp.locator(".ke-form dd, .ke-step, .ke-insp-foot button").all()) await contained(control, insp, false);
  if (stacked) {
    const m = (await main.boundingBox())!, i = (await insp.boundingBox())!;
    expect(i.y).toBeGreaterThanOrEqual(m.y + m.height - 1);
    expect(m.width).toBeGreaterThanOrEqual((await split.boundingBox())!.width - 2);
    const control = insp.locator("button, input").first();
    if (await control.count()) {
      await control.scrollIntoViewIfNeeded(); await contained(control); await contained(control, split); await contained(control, insp);
    }
  }
}

test("minimum desktop inventory and inspector containment", async ({ page }) => {
  await page.setViewportSize(SIZES[0]); await open(page);
  await page.locator(".ke-grid .ke-slot.full").first().click();
  await shot(page, "minimum-inventory");
  await inventory(page);
  await shot(page, "minimum-inspector");
});

test("Russian Deck Save, inspector and footer containment", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 }); await open(page, true, "ru");
  await shot(page, "russian-deck");
  await contained(page.locator('[data-deck-id="ed-save"]'));
  await inventory(page, true);
  for (const hint of await page.locator(".deck-hint").all()) await contained(hint, page.locator(".deck-hints"));
});

for (const deck of [false, true]) test(`toolbar contains two additional history actions ${deck ? "Deck" : "desktop"}`, async ({ page }) => {
  await page.setViewportSize(deck ? { width: 1280, height: 800 } : SIZES[0]); await open(page, deck, "ru");
  await page.evaluate(async (isDeck) => {
    const { useSession } = await import("/src/editor/kiln/session.js"); useSession.setState({ dirty: true });
    const toolbar = document.querySelector(isDeck ? ".dke-buttons" : ".k-page-actions")!;
    for (const label of ["↶ Отменить", "↷ Повторить"]) {
      const button = document.createElement("button"); button.className = isDeck ? "deck-btn" : "k-btn";
      button.textContent = label; toolbar.insertBefore(button, toolbar.firstChild);
      if (isDeck) { const hint = document.createElement("button"); hint.className = "deck-hint"; hint.textContent = label; document.querySelector(".deck-hints-left")!.append(hint); }
    }
  }, deck);
  await shot(page, `history-actions-${deck ? "deck" : "desktop"}`);
  const toolbar = page.locator(deck ? ".dke-buttons" : ".k-page-actions"); await contained(toolbar);
  for (const button of await toolbar.locator("button").all()) await contained(button, toolbar);
  if (!deck) expect((await page.locator(".k-page-head > div").first().boundingBox())!.width).toBeGreaterThanOrEqual((await page.locator(".k-page-head").boundingBox())!.width / 2);
  if (deck) for (const hint of await page.locator(".deck-hint").all()) await contained(hint, page.locator(".deck-hints"));
});

test("Deck inspector numeric control retains its visible focus ring", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 }); await open(page, true);
  const number = page.locator(".ke-insp .ke-step-v").first(); await number.focus();
  await shot(page, "deck-number-focus");
  await contained(number, page.locator(".ke-insp"), true, 6);
  expect(await number.evaluate((el) => getComputedStyle(el).outlineStyle)).toBe("solid");
});

test("long character name keeps its chevron beside the title", async ({ page }) => {
  await page.setViewportSize({ width: 1360, height: 860 }); await open(page, false, "ru");
  await page.evaluate(async () => {
    const { useSession } = await import("/src/editor/kiln/session.js");
    useSession.setState({ dirty: true, model: { ...useSession.getState().model, name: "Хранитель пламени с невероятно длинным именем" } });
  });
  await shot(page, "long-name");
  await contained(page.locator(".ke-who-btn"), page.locator(".k-page-head > div").first());
  const lastLine = await page.locator(".ke-who-btn").evaluate((el) => {
    const walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
    let node: Node | null, last: Node | null = null;
    while ((node = walker.nextNode())) if (node.textContent?.trim()) last = node;
    const range = document.createRange(); range.setStart(last!, last!.textContent!.length - 1); range.setEnd(last!, last!.textContent!.length);
    return range.getBoundingClientRect().toJSON();
  });
  const chevron = (await page.locator(".ke-who-btn svg").boundingBox())!;
  expect(chevron.y, "chevron must not wrap to a line of its own").toBeLessThan(lastLine.y + lastLine.height);
  expect(chevron.x).toBeGreaterThanOrEqual(lastLine.x + lastLine.width - 1);
});

test("backup filename is readable rather than clipped in its cell", async ({ page }) => {
  await open(page);
  await page.locator(".k-page-actions button").first().click();
  await expect(page.locator(".ke-bk-row").first()).toBeVisible();
  await shot(page, "backup-filenames");
  for (const row of await page.locator(".ke-bk-row").all()) {
    for (const cell of await row.locator(":scope > span").all()) await contained(cell, row);
    const filename = row.locator(".k-mono");
    expect(await filename.evaluate((el) => el.scrollWidth <= el.clientWidth + 1), "the useful filename is not ellipsized").toBe(true);
    expect(await filename.getAttribute("title")).toContain("character0.ksd");
  }
});

test("active Catalog count meets readable Hearth contrast", async ({ page }) => {
  await open(page); await page.locator(".ke-group-head button").first().click();
  await expect(page.locator(".ke-cat-grid .ke-slot").first()).toBeVisible();
  await shot(page, "catalog-count");
  const count = page.locator(".ke-cat-rail .k-sub.on .ke-cat-n");
  await contained(count, page.locator(".ke-cat-rail"));
  const contrast = await count.evaluate((el) => {
    const rgb = (s: string) => s.match(/[\d.]+/g)!.map(Number);
    let bg = [0, 0, 0];
    const ancestors: Element[] = []; for (let p: Element | null = el; p; p = p.parentElement) ancestors.unshift(p);
    for (const p of ancestors) { const c = rgb(getComputedStyle(p).backgroundColor), a = c[3] ?? 1; bg = bg.map((v, i) => c[i] * a + v * (1 - a)); }
    const luminance = (c: number[]) => c.slice(0, 3).map((x) => x / 255).map((x) => x <= .04045 ? x / 12.92 : ((x + .055) / 1.055) ** 2.4).reduce((s, x, i) => s + x * [.2126, .7152, .0722][i], 0);
    const fg = luminance(rgb(getComputedStyle(el).color)), back = luminance(bg);
    return (Math.max(fg, back) + .05) / (Math.min(fg, back) + .05);
  });
  expect(contrast).toBeGreaterThanOrEqual(4.5);
});

test("skill-total explanation stays compact and clear of zoom controls", async ({ page }) => {
  await page.setViewportSize(SIZES[0]); await open(page); await page.getByRole("tab").nth(2).click();
  await shot(page, "skill-total");
  const points = page.locator(".ke-tree-points"), zoom = page.locator(".ke-tree-zoom");
  await contained(points, page.locator(".ke-tree"));
  const p = (await points.boundingBox())!, z = (await zoom.boundingBox())!;
  expect(p.width).toBeLessThanOrEqual(460);
  expect(p.x + p.width).toBeLessThan(z.x - 8);
});

test("missing-art notice wraps inside the minimum editor viewport", async ({ page }) => {
  await page.setViewportSize(SIZES[0]); await page.route("**/assets-local/editor/**", (r) => r.abort());
  await open(page, false, "ru"); await expect(page.locator(".kiln-editor-hint")).toBeVisible();
  await shot(page, "missing-art");
  await contained(page.locator(".kiln-editor-hint"), page.locator(".kiln-editor"));
  const hint = (await page.locator(".kiln-editor-hint").boundingBox())!, editor = (await page.locator(".ke.k-page").boundingBox())!;
  expect(hint.y, "the art status must not cover editor content").toBeGreaterThanOrEqual(editor.y + editor.height - 1);
  await inventory(page);
});

for (const locale of LOCALES) {
  for (const size of SIZES) test(`desktop containment ${locale} ${size.width}x${size.height}`, async ({ page }) => {
    await page.setViewportSize(size);
    if (size.width === 1024) await page.route("**/assets-local/editor/**", (r) => r.abort());
    await open(page, false, locale);
    if (size.width === 1024) await contained(page.locator(".kiln-editor-hint"), page.locator(".kiln-editor"));
    await page.locator(".ke-grid .ke-slot.full").first().click(); await inventory(page);
    await contained(page.locator(".k-page-actions"), page.locator(".k-page-head"));
    await page.getByRole("tab").nth(0).click(); await contained(page.locator(".ke-gear"), page.locator(".ke-main"), false);
    await page.getByRole("tab").nth(2).click(); await contained(page.locator(".ke-tree-points"), page.locator(".ke-tree"));
    await page.getByRole("tab").nth(3).click(); await contained(page.locator(".ke-know-bar"), page.locator(".ke-know"));
    await page.locator(".k-page-actions button").first().click();
    await expect(page.locator(".ke-bk-row").first()).toBeVisible();
    await page.locator(".ke-bk-row button").first().click();
    for (const row of await page.locator(".ke-bk-row").all()) for (const cell of await row.locator(":scope > span").all()) await contained(cell, row);
    await page.keyboard.press("Escape");
    await page.getByRole("tab").nth(1).click(); await page.locator(".ke-group-head button").first().click();
    await contained(page.locator(".ke-cat"), page.getByRole("dialog"));
    for (const panel of await page.locator(".ke-cat > div").all()) await contained(panel, page.locator(".ke-cat"));
    await shot(page, `desktop-${locale}-${size.width}`);
  });
  test(`Deck containment and focus rings ${locale}`, async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 }); await open(page, true, locale);
    await inventory(page, true);
    for (const button of await page.locator(".dke-buttons button, .dke-tabs button").all()) await contained(button);
    for (const hint of await page.locator(".deck-hint").all()) await contained(hint, page.locator(".deck-hints"));
    // The focused slot has room for the Deck's 3px outline and 3px offset, not just its content box.
    const slot = page.locator(".ke-grid .ke-slot.full").first(); await slot.focus();
    await contained(slot, page.locator(".dke-body"), true, 6);
    expect(await slot.evaluate((el) => getComputedStyle(el).outlineStyle)).toBe("solid");
    await shot(page, `deck-${locale}`);
  });
}
