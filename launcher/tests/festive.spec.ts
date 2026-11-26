// The festive season (shell/season.ts, shell/Festive.tsx): the mantel over the mouth on Play, the threshold's diyas, the
// garland on the sheets and the snow in the fire (Stage.tsx FRAG_FESTIVE), from the Monday after Thanksgiving weekend to
// 7 January. Off under automation; ?theme=festive / ?theme=normal force it, ?theme=auto lets the date decide (with the
// page's clock set). On by default in the season, switched off in Settings for the rest of it. Never with the turkey.
// Outside the season the launcher compiles and draws the same fire as every other day.
import { expect, test, type Page } from "@playwright/test";

// a smaller window than the suite's, the player's zone fixed (the dates are local)
test.use({ viewport: { width: 1024, height: 640 }, timezoneId: "America/New_York" });

type Rec = { __frags: string[]; __draws: number };
async function record(page: Page) {
  await page.addInitScript(() => {
    const w = window as unknown as Rec;
    w.__frags = [];
    w.__draws = 0;
    const P = WebGLRenderingContext.prototype;
    const src = P.shaderSource, draw = P.drawArrays;
    P.shaderSource = function (s: WebGLShader, text: string) {
      if (text.includes("gl_FragColor")) w.__frags.push(text);
      return src.call(this, s, text);
    };
    P.drawArrays = function (m: number, f: number, c: number) {
      w.__draws += 1;
      return draw.call(this, m, f, c);
    };
  });
}
const rec = (page: Page) => page.evaluate(() => { const w = window as unknown as Rec; return { frags: w.__frags.slice(), draws: w.__draws }; });
const hasGl = (page: Page) => page.evaluate(() => !!document.createElement("canvas").getContext("webgl"));
const shaders = (page: Page) => page.evaluate(async () => {
  const m = await import("/src/shell/Stage.tsx");
  return { plain: m.FRAG as string, festive: m.FRAG_FESTIVE as string };
});

async function open(page: Page, query: string, route = "/") {
  await page.goto(`/${query}#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
/** 00:00 local in New York as an instant (EST, UTC-5, over these dates). */
const ny = (y: number, mo: number, d: number, h = 0, mi = 0) => new Date(Date.UTC(y, mo - 1, d, h + 5, mi));

test("under automation, without a word in the address: every day's launcher, no Settings row", async ({ page }) => {
  await record(page);
  await open(page);
  await expect(page.locator(".k-app")).not.toHaveAttribute("data-theme", /.+/);
  await expect(page.locator(".k-fest-mantel")).toHaveCount(0);
  if (await hasGl(page)) {
    await expect(page.locator(".k-app")).toHaveAttribute("data-gl", "1");
    expect([...new Set((await rec(page)).frags)]).toEqual([(await shaders(page)).plain]);
  }
  await page.goto("/#/settings");
  await expect(page.locator('[data-setting="fire"]')).toBeVisible();
  await expect(page.locator('[data-setting="festive"]')).toHaveCount(0);
});

test("forced on: the mantel and its wish, the threshold's diyas (no clicks taken), the snowy fire, the sheets' garland", async ({ page }) => {
  await record(page);
  await open(page, "?theme=festive");
  await expect(page.locator(".k-app")).toHaveAttribute("data-theme", "festive");
  await expect(page.locator(".k-fest-mantel").first()).toBeVisible();
  await expect(page.locator(".kf-greet-a")).toHaveText("Warm wishes for the season");
  // the symbols: a hanukkiah's nine lights and a kinara's seven, a flame for each lamp
  expect(await page.locator(".k-fest-mantel:not(.k-fest-hearth) [data-fl]").count()).toBe(9 + 7 + 4);
  // the Play button still takes the click where the diyas stand in front of it
  const b = (await page.locator(".k-door").boundingBox())!;
  expect(await page.evaluate(([x, y]) => document.elementFromPoint(x, y)?.closest(".k-door") !== null, [b.x + 14, b.y + b.height - 8])).toBe(true);
  if (await hasGl(page)) {
    await expect(page.locator(".k-app")).toHaveAttribute("data-gl", "1");
    expect([...new Set((await rec(page)).frags)]).toEqual([(await shaders(page)).festive]);
  }
  await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Servers" }).click();
  await expect(page.locator(".k-sheet .k-fest-rim")).toBeVisible();
  await expect(page.locator(".k-fest-mantel").first()).toBeHidden();
});

test("forced off: every day's fire, byte for byte", async ({ page }) => {
  await record(page);
  await open(page, "?theme=normal");
  await expect(page.locator(".k-fest-mantel")).toHaveCount(0);
  await expect(page.locator(".k-app")).not.toHaveAttribute("data-theme", /.+/);
  test.skip(!(await hasGl(page)), "no WebGL in this browser: the fire never starts");
  await expect(page.locator(".k-app")).toHaveAttribute("data-gl", "1");
  const frags = [...new Set((await rec(page)).frags)];
  expect(frags).toEqual([(await shaders(page)).plain]);
  expect(frags[0]).not.toContain("snow(");
});

test("the turkey wins: never both", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await open(page, "?turkey=1&theme=festive");
  await expect(page.locator(".k-fest-mantel")).toHaveCount(0);
  await expect(page.locator(".k-app")).not.toHaveAttribute("data-theme", /.+/);
});

test("reduced motion: a still frame (no snow falling, no sway, no flicker), the mantel drawn", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await record(page);
  await open(page, "?theme=festive");
  await expect(page.locator(".k-fest-mantel").first()).toBeVisible();
  const sway = page.locator("[data-sway]").first(), flame = page.locator("[data-fl]").first();
  await page.waitForTimeout(700);
  const a = { sway: await sway.getAttribute("transform"), flame: await flame.getAttribute("transform"), draws: (await rec(page)).draws };
  await page.waitForTimeout(900);
  expect(await sway.getAttribute("transform")).toBe(a.sway);
  expect(await flame.getAttribute("transform")).toBe(a.flame);
  expect((await rec(page)).draws).toBe(a.draws);
});

test("the date decides (?theme=auto): on by default in December, off in Settings, still off after a restart", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.clock.install({ time: ny(2026, 12, 10, 20) });
  await open(page, "?theme=auto", "/settings");
  await expect(page.locator(".k-app")).toHaveAttribute("data-theme", "festive");
  const row = page.locator('[data-setting="festive"]');
  await expect(row).toContainText("Festive season");
  await expect(page.getByRole("switch", { name: /^Festive season/ })).toBeChecked();
  await row.locator("label.k-toggle").click();
  await expect(page.locator(".k-app")).not.toHaveAttribute("data-theme", /.+/);
  expect(await page.evaluate(() => localStorage.getItem("kiln.festive.off"))).toBe("2026");
  await page.reload();
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.getByRole("switch", { name: /^Festive season/ })).not.toBeChecked();
  await page.evaluate(() => { location.hash = "#/"; });
  await expect(page.locator(".k-door")).toBeVisible();
  await expect(page.locator(".k-fest-mantel")).toHaveCount(0);
  // on again: at once, and the opt-out is gone
  await page.evaluate(() => { location.hash = "#/settings"; });
  await row.locator("label.k-toggle").click();
  await expect(page.locator(".k-app")).toHaveAttribute("data-theme", "festive");
  expect(await page.evaluate(() => localStorage.getItem("kiln.festive.off"))).toBeNull();
});

test("a launcher left open: the turkey gives way at Monday 00:00, the festive look goes at 8 January 00:00", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.clock.install({ time: ny(2026, 11, 29, 23, 55) });
  await open(page, "?theme=auto");
  await expect(page.locator(".k-fest-mantel")).toHaveCount(0);
  // Sunday night: the turkey (its greeting shows where the fire burns)
  if (await hasGl(page)) await expect(page.locator(".k-thanks")).toBeVisible();
  await page.clock.fastForward("10:00");
  await expect(page.locator(".k-fest-mantel").first()).toBeVisible();
  await expect(page.locator(".k-thanks")).toHaveCount(0);
  await page.clock.setSystemTime(ny(2027, 1, 7, 23, 55));
  await page.clock.fastForward("10:00");
  await expect(page.locator(".k-fest-mantel")).toHaveCount(0);
  await expect(page.locator(".k-app")).not.toHaveAttribute("data-theme", /.+/);
});

test("the wish in the player's language, kept between the lintel's joints", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await open(page, "?theme=festive", "/settings");
  await page.locator("#st-lang").selectOption("pl");
  await expect.poll(() => page.evaluate(() => document.documentElement.lang)).toBe("pl");
  await page.evaluate(() => { location.hash = "#/"; });
  const greet = page.locator(".kf-greet-a");
  await expect(greet).toHaveText("Ciepłych świąt");
  expect(await greet.evaluate((el) => (el as SVGTextElement).getComputedTextLength())).toBeLessThanOrEqual(601);
});
