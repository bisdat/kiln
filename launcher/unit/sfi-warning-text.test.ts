// the warning before Play: what the dialog says (src/shell/sfiWarnings.ts): the strongest
// lead (save data first), each warning in full (reasons, data/Lua/DLL parts, the dependency chain, the folder, an
// earlier start), the base the plan was made from in its exact words with Restore and Verify apart from the warnings,
// and the same words in all 11 locales.
import { expect, test } from "bun:test";
import type { StartWarning } from "../src/api/commands";
import { advisoryText, packageLabel, strongest, warningText } from "../src/shell/sfiWarnings";
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

const pkg = (o: Partial<NonNullable<StartWarning["package"]>> = {}): StartWarning => ({
  kind: "package-skipped", code: "dependency-skipped", message: "m", severity: "warning", causedBy: ["lib-b", "lib-a"],
  package: { id: "needy", version: "1.2.0", root: "mods/needy", saveSensitive: false, data: true, lua: true, dlls: ["needy.dll", "extra.dll"],
    reasons: [{ code: "dependency-skipped", message: "needs lib-b, which does not run", severity: "warning" }, { code: "x", message: "second reason", severity: "warning" }], ...o },
});
const off: StartWarning = { kind: "previously-active-off", code: "save-sensitive-now-off", message: "m", severity: "save-warning",
  previous: { id: "ammo", version: "0.8.2", name: "Quiver", basis: "policy", fromLaunchId: "00000000000000ab" } };

test("the lead: save data before anything else, then an incomplete check, then plain", () => {
  expect(strongest([pkg()])).toBe("plain");
  expect(strongest([pkg(), { ...pkg(), severity: "infrastructure-error" }])).toBe("check");
  expect(strongest([pkg(), { ...pkg(), severity: "infrastructure-error" }, off])).toBe("save");
  expect(strongest([pkg({ saveSensitive: true })])).toBe("save");
});

test("a skipped package: every reason, its parts, its data not applied, the chain, its folder", () => {
  const w = warningText(pkg());
  expect(w.title).toBe("needy 1.2.0 does not run");
  expect(w.lines).toEqual([
    "needs lib-b, which does not run",
    "second reason",
    "Parts: startup data · Lua · DLLs: needy.dll, extra.dll",
    "Its startup data is not applied this start.",
    "Because of: lib-b → lib-a",
    "Folder: mods/needy",
  ]);
  expect(w.save).toBe(false);
  const s = warningText(pkg({ saveSensitive: true, data: false, dlls: [] }));
  expect(s.save).toBe(true);
  expect(s.lines.at(-1)).toBe("It keeps data in your saves: do not save a world while it is off.");
  expect(s.lines.some((l) => l.startsWith("Parts:") && l.includes("startup data"))).toBe(false);
});

test("a skipped package active before says so, as a save warning", () => {
  const w = warningText({ ...pkg(), severity: "save-warning", previous: { id: "needy", basis: "unknown-mixed", fromLaunchId: "00000000000000ad" } });
  expect(w.save).toBe(true);
  expect(w.lines).toContain("It was active at an earlier start (00000000000000ad).");
  expect(w.lines.at(-1)).toBe("It keeps data in your saves: do not save a world while it is off.");
});

test("previously active, off now: its name, the earlier start, why it counts, the save line", () => {
  const w = warningText(off);
  expect(w.title).toBe("Quiver (ammo 0.8.2) is off now");
  expect(w.save).toBe(true);
  expect(w.lines).toEqual([
    "It does not run this start.",
    "It was active at an earlier start (00000000000000ab).",
    "Kiln's list of mods that keep save data names it.",
    "It keeps data in your saves: do not save a world while it is off.",
  ]);
  expect(warningText({ ...off, previous: { id: "x", basis: "unknown-mixed" } }).lines.slice(1, 3)).toEqual([
    "It was active at an earlier start (start unknown).", "It has both game data and code, so it may keep save data.",
  ]);
});

test("start refused, the inspector, other plan warnings", () => {
  const r = warningText({ kind: "start-refused", code: "interpreter-held", message: "held for tests", severity: "warning", packages: ["a", "b"] });
  expect([r.title, ...r.lines]).toEqual(["No ShroudForge mod runs this start", "held for tests", "Not started: a, b"]);
  const i = warningText({ kind: "plan-warning", code: "inspector-unresolved", message: "cannot be used (gone)", severity: "warning" });
  expect([i.title, ...i.lines]).toEqual(["Kiln's archive inspector cannot be used", "cannot be used (gone)"]);
  const p = warningText({ kind: "plan-warning", code: "rules-unreadable", message: "unreadable", severity: "warning", causedBy: ["x"] });
  expect([p.title, ...p.lines]).toEqual(["Launch plan warning", "unreadable", "About: x"]);
});

test("the base origin: the exact label, then Restore and Steam's Verify", () => {
  const a = advisoryText({ kind: "base-origin", code: "first-managed-unclassified", message: "unclassified: game files as found when Kiln first managed this install", severity: "info" });
  expect(a.lines).toEqual([
    "unclassified: game files as found when Kiln first managed this install",
    "To go back to exactly these files, switch off the mods that change the game's data and start the game through Kiln once (Logs, Engines: Restore the game's data).",
    "For the game's files as Steam ships them, use Verify integrity of game files in Steam.",
  ]);
  expect(advisoryText({ kind: "cost", code: "reinspects-every-play", message: "slow", severity: "info" }).title).toBe("Slower start");
});

test("every advisory has its own key, two of one kind and code too; a keyboard shortcut note names its package", () => {
  const a: StartWarning = { kind: "plan-warning", code: "inventory", message: "one", severity: "info" };
  const keys = [a, { ...a, message: "two" }].map(advisoryText).map((n) => n.key);
  expect(new Set(keys).size).toBe(2);
  const k = advisoryText({ kind: "plan-warning", code: "keybinds-unsupported", message: "m", severity: "info", packages: ["hotkeys 1.0.0"] });
  expect(k.title).toBe("Keyboard shortcuts");
  expect(k.lines).toEqual(["hotkeys 1.0.0: its keyboard shortcuts do not work with Kiln's interpreter yet. The rest of the mod runs."]);
});

test("labels", () => {
  expect(packageLabel("ammo", "0.8.2", "Quiver")).toBe("Quiver (ammo 0.8.2)");
  expect(packageLabel("ammo", "0.8.2", "ammo")).toBe("ammo 0.8.2");
  expect(packageLabel("ammo")).toBe("ammo");
});

test("every locale has the dialog's words, with the same placeholders", () => {
  const flat = (o: unknown, p = ""): [string, string][] => Object.entries(o as Record<string, unknown>)
    .flatMap(([k, v]) => typeof v === "object" ? flat(v, `${p}${k}.`) : [[`${p}${k}`, String(v)] as [string, string]]);
  const ph = (s: string) => (s.match(/\{[a-z]+\}/gi) ?? []).sort().join();
  const base = Object.fromEntries(flat((en as Record<string, Record<string, unknown>>)["sfBackend"].startWarning));
  expect(base.continue).toBe("Continue without these mods");
  for (const loc of [de, es, fr, it, ja, ko, pl, ptBR, ru, zhCN]) {
    const mine = Object.fromEntries(flat((loc as Record<string, Record<string, unknown>>)["sfBackend"].startWarning));
    expect(Object.keys(mine).sort()).toEqual(Object.keys(base).sort());
    for (const k of Object.keys(base)) expect(ph(mine[k])).toBe(ph(base[k]));
  }
});
