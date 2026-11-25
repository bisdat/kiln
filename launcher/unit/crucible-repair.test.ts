// Download missing mods' words (src/crucible/repair.ts): the button, the line before Load it, the count; English only,
// so another language falls back to it for these keys.
import { afterEach, expect, test } from "bun:test";
import { missingCount, missingLine, repairLabel } from "../src/crucible/repair";
import { setLanguage, setUserLocales, t } from "../src/i18n";
import type { PackLacking } from "../src/api/commands";
import de from "../src/i18n/de.json";

// German as a data-folder file (setUserLocales), so no other test file meets it as a built-in language
afterEach(() => { setUserLocales([]); setLanguage("en"); });

const MB = 1_048_576;
const ref = (id: string) => ({ kind: "kiln" as const, id, version: "1.0.0", hash: "kdh1:" + id });
const lack = (ids: string[], names: string[], download: number): PackLacking => ({ version: "1.0.0", missing: ids.map(ref), names, download });

test("the button names how many and how much; from a file, no download", () => {
  expect(repairLabel(lack(["big-stacks"], ["Bigger Stacks"], 1.1 * MB))).toBe("Download the missing mod (1.1 MB)");
  expect(repairLabel(lack(["a", "b"], ["A", "B"], 13 * MB))).toBe("Download missing mods (2, 13 MB)");
  expect(repairLabel(lack(["a", "b"], ["A", "B"], 13 * MB), "file")).toBe("Add the 2 missing mods from this file");
  expect(repairLabel(lack(["a"], ["A"], 0), "file")).toBe("Add the missing mod from this file");
});

test("the line before Load it: the mods by name, the pack's own layer by its role", () => {
  expect(missingLine(lack(["big-stacks"], ["Bigger Stacks"], MB))).toBe("Bigger Stacks (pack version 1.0.0) is no longer in your Library. Load it would start the game without it.");
  expect(missingLine(lack(["a", "b", "pack.ashen"], ["Lantern Glow", "Night Sky", "pack.ashen"], MB)))
    .toBe("Lantern Glow, Night Sky (pack version 1.0.0) are no longer in your Library. Load it would start the game without them. The pack's own settings are missing too.");
  expect(missingLine(lack(["pack.ashen"], ["pack.ashen"], MB))).toBe("The pack's own settings (pack version 1.0.0) are no longer in your Library. Load it would start the game without them.");
  expect(missingCount({ repair: lack(["a", "b"], ["A", "B"], 1) })).toBe(2);
  expect(missingCount({ repair: null })).toBe(0);
  expect(missingCount(undefined)).toBe(0);
});

test("only English has the words: German falls back to them, its own words stay", () => {
  setUserLocales([{ tag: "de", file: de }]);
  setLanguage("de");
  expect(t("crucible.preview.loadIt")).toBe("Laden");
  expect(repairLabel(lack(["a", "b"], ["A", "B"], 2 * MB))).toBe("Download missing mods (2, 2.0 MB)");
  expect(t("crucible.repair.title", { count: 1 })).toBe("A mod of this pack is missing");
});
