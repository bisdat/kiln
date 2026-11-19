// The sheet stays cheap while the fire burns (inbox 94: every page but Play lagged, a page switch took about a second
// and typing 250 to 750 ms). The flicker (--fire, shell/Stage.tsx) and the torch (--mx, --my, shell/AppShell.tsx) write
// only onto leaves: an element with children under it would restyle and repaint the whole page on every write. With
// the fire off (Settings' Animated fire, drawing without the GPU) or reduced motion there are no writes at all, and
// without the GPU a lit flame stands still.
// The last test types into a sheet page in WebKit (the engine of the Linux launcher) while the pointer moves; it runs
// where Playwright's WebKit starts (KILN_WEBKIT names another build; not on distributions it does not support).
import { expect, test, webkit, type Page } from "@playwright/test";

type Write = { name: string; cls: string; kids: number };

/** Records every write of the moving properties with the element it went onto (and how many element children it has). */
async function countWrites(page: Page) {
  await page.addInitScript(() => {
    const w = window as unknown as { __writes: Write[] };
    w.__writes = [];
    const desc = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "style")!;
    const owner = new WeakMap<CSSStyleDeclaration, HTMLElement>();
    Object.defineProperty(HTMLElement.prototype, "style", {
      configurable: true,
      get(this: HTMLElement) { const s = desc.get!.call(this) as CSSStyleDeclaration; owner.set(s, this); return s; },
      set: desc.set,
    });
    const set = CSSStyleDeclaration.prototype.setProperty;
    CSSStyleDeclaration.prototype.setProperty = function (name: string, value: string | null, prio?: string) {
      if (/^--(fire|mx|my|k)$/.test(name)) {
        const el = owner.get(this);
        w.__writes.push({ name, cls: el ? String(el.className) : "?", kids: el ? el.childElementCount : -1 });
      }
      return set.call(this, name, value, prio);
    };
  });
}

const writes = (page: Page) => page.evaluate(() => (window as unknown as { __writes: Write[] }).__writes.splice(0));

async function open(page: Page, route: string) {
  await page.goto(`/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

/** The pointer wanders over the sheet for about a second, then a field gets a few letters. */
async function wander(page: Page) {
  for (let i = 0; i < 40; i++) await page.mouse.move(300 + ((i * 37) % 700), 220 + ((i * 53) % 420));
  await page.waitForTimeout(700);
}

const hasGl = (page: Page) => page.evaluate(() => !!document.createElement("canvas").getContext("webgl"));

test("the flicker and the torch write only onto leaves, never onto an element with the page under it", async ({ page }) => {
  await countWrites(page);
  await open(page, "/settings");
  test.skip(!(await hasGl(page)), "no WebGL in this browser: the fire, the flicker and the torch never start");
  await expect(page.locator(".k-app")).toHaveAttribute("data-gl", "1");
  await wander(page);
  await page.locator("#st-keep").fill("12");
  // Play (the title flickers) and back (the sheet's edge does, the mouth springs)
  await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Play" }).click();
  await page.waitForTimeout(800);
  await wander(page);
  await page.locator(".k-head-link", { hasText: "Settings" }).click();
  await page.waitForTimeout(800);
  await wander(page);
  const all = await writes(page);
  const by = (n: string) => all.filter((w) => w.name === n);
  // they did run
  expect(by("--fire").length).toBeGreaterThan(10);
  expect(by("--mx").length).toBeGreaterThan(5);
  expect(by("--my").length).toBe(by("--mx").length);
  expect(by("--k").length).toBeGreaterThan(0);
  // and only onto their leaves
  const onParents = all.filter((w) => w.name !== "--k" && w.kids !== 0);
  expect(onParents).toEqual([]);
  expect([...new Set(by("--fire").map((w) => w.cls.split(" ")[0]))].sort()).toEqual(["k-sheet-rim", "k-title"]);
  expect([...new Set([...by("--mx"), ...by("--my")].map((w) => w.cls))]).toEqual(["k-torch"]);
  // the mouth's spring fades the label, not the whole window
  expect([...new Set(by("--k").map((w) => w.cls))]).toEqual(["k-door-lab"]);
  // nothing under the sheet reads them: they do not inherit
  const inherited = await page.evaluate(() => {
    const el = document.querySelector(".k-sheet-in") as HTMLElement;
    const s = getComputedStyle(el);
    return [s.getPropertyValue("--fire").trim(), s.getPropertyValue("--mx").trim()];
  });
  expect(inherited).toEqual(["0.5", "0px"]);
});

test("the sheet looks as it did: the lit top edge and the torch's light are where the sheet drew them", async ({ page }) => {
  await open(page, "/settings");
  const look = await page.evaluate(() => {
    const sheet = document.querySelector(".k-sheet") as HTMLElement;
    const rim = document.querySelector(".k-sheet-rim") as HTMLElement;
    const torch = document.querySelector(".k-torch") as HTMLElement;
    const sb = sheet.getBoundingClientRect(), rb = rim.getBoundingClientRect(), tb = torch.getBoundingClientRect();
    return {
      sheetShadow: getComputedStyle(sheet).boxShadow, rimShadow: getComputedStyle(rim).boxShadow, rimRadius: getComputedStyle(rim).borderRadius,
      rim: [rb.left - sb.left, rb.top - sb.top, rb.width - sb.width], rimKids: rim.childElementCount, torchKids: torch.childElementCount,
      // the light's centre: 50% across and 30% down the sheet, less the sheet's offset (base.css before inbox 94)
      torchCentre: [Math.round(tb.left + tb.width / 2 - sb.left - (sb.width * 0.5 - 24)), Math.round(tb.top + tb.height / 2 - sb.top - (sb.height * 0.3 - 76))],
      before: getComputedStyle(sheet, "::before").content,
    };
  });
  expect(look.sheetShadow).toBe("rgba(0, 0, 0, 0.6) 0px -24px 70px 0px");
  expect(look.rimShadow).toMatch(/^rgba\(255, 190, 140, 0\.\d+\) 0px 1px 0px 0px inset$/);
  expect(look.rimRadius).toBe("20px 20px 0px 0px");
  expect(look.rim).toEqual([0, 0, 0]);
  expect([look.rimKids, look.torchKids]).toEqual([0, 0]);
  expect(look.torchCentre).toEqual([0, 0]);
  expect(look.before).toBe("none");
  // the light follows the pointer once it moves (where the fire burns): centred on it
  if (!(await hasGl(page))) return;
  await page.mouse.move(640, 420);
  await page.mouse.move(650, 430);
  await expect.poll(() => page.locator(".k-torch").evaluate((el) => {
    const b = el.getBoundingClientRect();
    return [Math.round(b.left + b.width / 2), Math.round(b.top + b.height / 2)];
  })).toEqual([650, 430]);
});

test("with reduced motion nothing is written while the pointer moves and the page stands", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await countWrites(page);
  await open(page, "/settings");
  await page.waitForTimeout(600);
  await writes(page); // the mouth placed once on the way in
  await wander(page);
  await page.locator("#st-keep").fill("12");
  expect((await writes(page)).filter((w) => w.name !== "--k")).toEqual([]);
});

test("with Animated fire off nothing is written: no flicker, no torch", async ({ page }) => {
  await countWrites(page);
  await open(page, "/settings");
  const fire = page.getByRole("switch", { name: /^Animated fire/ });
  await expect(fire).toBeChecked();
  await page.getByText("Animated fire", { exact: true }).click();
  await expect(fire).not.toBeChecked();
  await expect(page.locator(".k-app")).not.toHaveAttribute("data-gl");
  await page.waitForTimeout(300);
  await writes(page);
  await wander(page);
  await page.locator("#st-keep").fill("12");
  await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Play" }).click();
  await wander(page);
  expect(await writes(page)).toEqual([]);
});

test("drawing without the GPU: no fire loop, no flicker, no torch, and lit flames stand still", async ({ page }) => {
  await countWrites(page);
  await page.goto("/?render=software#/settings");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.locator(".k-app")).not.toHaveAttribute("data-gl");
  await wander(page);
  await page.locator("#st-keep").fill("12");
  expect(await writes(page)).toEqual([]);
  // a lit flame stands still: its flicker repainted it and its glow every frame on the CPU
  await page.evaluate(() => { location.hash = "#/mods/order"; });
  const flame = page.locator(".k-flame.on svg").first();
  await expect(flame).toBeVisible();
  await expect(flame).toHaveCSS("animation-name", "none");
});

test("with the GPU a lit flame still flickers", async ({ page }) => {
  await open(page, "/mods/order");
  const flame = page.locator(".k-flame.on svg").first();
  await expect(flame).toBeVisible();
  await expect(flame).toHaveCSS("animation-name", "k-lick");
});

test("WebKit: typing into a sheet page stays quick while the fire burns and the pointer moves", async ({ baseURL }) => {
  let browser;
  try {
    browser = await webkit.launch({ executablePath: process.env.KILN_WEBKIT || undefined });
  } catch (e) {
    test.skip(true, `Playwright's WebKit does not start here: ${String(e).split("\n")[0]}`);
    return;
  }
  try {
    const page = await browser.newPage({ viewport: { width: 1360, height: 860 } });
    await page.goto(`${baseURL}/#/settings`);
    await page.locator(".k-splash").waitFor({ state: "detached" });
    await page.waitForTimeout(800);
    await page.locator("#st-keep").click();
    await page.evaluate(() => {
      const w = window as unknown as { __lat: number[] };
      w.__lat = [];
      // a key's time until the frame that shows it has been drawn
      document.addEventListener("keydown", (e) => { const t = e.timeStamp; requestAnimationFrame(() => setTimeout(() => w.__lat.push(performance.now() - t), 0)); }, true);
    });
    for (let i = 0; i < 24; i++) {
      for (let j = 0; j < 4; j++) await page.mouse.move(300 + ((i * 4 + j) * 29) % 700, 240 + ((i * 4 + j) * 47) % 400);
      await page.keyboard.press(String(i % 10));
    }
    await page.waitForTimeout(300);
    const lat = (await page.evaluate(() => (window as unknown as { __lat: number[] }).__lat)).sort((a, b) => a - b);
    expect(lat.length).toBe(24);
    const median = lat[Math.floor(lat.length / 2)];
    const gl = await page.locator(".k-app").getAttribute("data-gl");
    console.log(`WebKit typing: median ${median.toFixed(1)} ms, slowest ${lat.at(-1)!.toFixed(1)} ms (the fire ${gl ? "burned" : "was off: no WebGL"})`);
    expect(median).toBeLessThan(100);
  } finally {
    await browser.close();
  }
});
