// The Conflicts page goes through the launcher's i18n like every other page: its header word, its page text and the
// labels the report parser makes (certainty, hidden fields, freshness). Native reasons and conflict texts stay as written.
import { afterAll, describe, expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import de from "../src/i18n/de.json";
import { registerLocale, setLanguage } from "../src/i18n";
import { NAV_MAIN } from "../src/shell/nav";
import { parseRegistryReport, reportFreshness } from "../src/registry/report";
import { registryFixture } from "../tests/fixtures/registryReport";

afterAll(() => setLanguage("en"));

describe("registry conflicts i18n", () => {
  test("the header word follows the language", () => {
    registerLocale("de", de);
    setLanguage("de");
    expect(NAV_MAIN.find((n) => n.to === "/conflicts")?.label).toBe("Konflikte");
    setLanguage("en");
    expect(NAV_MAIN.find((n) => n.to === "/conflicts")?.label).toBe("Conflicts");
  });
  test("parser labels and freshness come from the locale; native reasons stay as written", () => {
    registerLocale("de", { ...de, registry: { certainty: { certain: "Sicher" }, fresh: { stopped: "Das Spiel läuft nicht." } } });
    setLanguage("de");
    const r = parseRegistryReport(JSON.stringify(registryFixture()));
    expect(r.conflicts[0].certainty).toBe("Sicher");
    expect(r.refused[0].reason).toBe(registryFixture().refused[0].reason);
    expect(reportFreshness(new Date().toISOString(), { running: false })).toBe("Das Spiel läuft nicht.");
    setLanguage("en");
    registerLocale("de", de);
  });
  test("the page has no English text outside en.json", () => {
    const page = readFileSync(join(import.meta.dir, "../src/registry/RegistryConflictsPage.tsx"), "utf8");
    for (const s of ["Registry conflicts", "Refused edits", "Conflicts and drift", "Fields and writer chains", "Refresh report", "Partial report", "Stale snapshot", "Previous", "Winner:"])
      expect(page).not.toContain(s);
  });
});
