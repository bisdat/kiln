// The save editor's words in the launcher's languages (src/editor, docs/kiln/translating.md): upstream's Portuguese keys
// (src/editor/src/i18n/launcher-keys.json) answer from the launcher's tables in the launcher's language, and every
// shipped language has every editor text (the editor is never half English), with Russian and Polish plurals whole.
import { afterAll, describe, expect, test } from "bun:test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { flatten, pluralBase } from "../src/i18n/format";
import { registerLocale, setLanguage, t } from "../src/i18n";
import { t as editorT, localeAtual } from "../src/editor/src/i18n.js";
import bridge from "../src/editor/src/i18n/launcher-keys.json";

const dir = join(import.meta.dir, "..", "src", "i18n");
const read = (tag: string) => JSON.parse(readFileSync(join(dir, `${tag}.json`), "utf8"));
const tags = readdirSync(dir).filter((f) => f.endsWith(".json") && f !== "en.json").map((f) => f.replace(/\.json$/, ""));
const editorBases = (tbl: Record<string, string>) => new Set(Object.keys(tbl).filter((k) => k.startsWith("editor.")).map(pluralBase));

afterAll(() => setLanguage("en"));

describe("save editor i18n", () => {
  test("every shipped language has every editor text", () => {
    const want = editorBases(flatten(read("en")));
    expect(want.size).toBeGreaterThan(200);
    for (const tag of tags) {
      const have = editorBases(flatten(read(tag)));
      expect([...want].filter((k) => !have.has(k)), tag).toEqual([]);
    }
  });

  test("Russian and Polish plurals have one, few and many", () => {
    for (const tag of ["ru", "pl"]) {
      const tbl = flatten(read(tag));
      const plurals = new Set(Object.keys(tbl).filter((k) => k.startsWith("editor.") && pluralBase(k) !== k).map(pluralBase));
      expect(plurals.size).toBeGreaterThan(5);
      for (const b of plurals) for (const f of ["one", "few", "many"]) expect(tbl[`${b}.${f}`], `${tag} ${b}.${f}`).toBeDefined();
    }
  });

  test("upstream's Portuguese keys answer in the launcher's language", () => {
    const ru = read("ru");
    registerLocale("ru", ru);
    setLanguage("en");
    expect(editorT("Lendário")).toBe("Legendary");
    expect(editorT("Pontos em uso ({usados}) passam do total do personagem ({total})", { usados: 12, total: 10 })).toBe("Points in use (12) exceed the character total (10)");
    setLanguage("ru");
    expect(editorT("Lendário")).toBe(ru.editor.game.rarity.legendary);
    expect(editorT("Força")).toBe(ru.editor.game.attribute.strength);
    expect(editorT("Item desconhecido (0x{h})", { h: "0000beef" })).toContain("0000beef");
    expect(localeAtual()).toBe("ru");
    // a text the bridge does not list keeps upstream's English (en.json of the editor)
    expect(editorT("Coleção")).toBe(ru.editor.game.category.collectible);
    expect(editorT("Servidor")).toBe("Server");
  });

  test("plural forms pick by count (Russian 1, 3, 5, 21)", () => {
    registerLocale("ru", read("ru"));
    setLanguage("ru");
    const forms = [1, 3, 5, 21].map((n) => t("editor.catalog.count", { count: n }));
    expect(forms[0]).toMatch(/^1 /);
    expect(new Set(forms.map((f) => f.replace(/^\d+ /, ""))).size).toBe(3);
    expect(forms[3].replace(/^21 /, "")).toBe(forms[0].replace(/^1 /, ""));
  });

  test("each bridged key exists in en.json", () => {
    const en = flatten(read("en"));
    for (const k of Object.values(bridge as Record<string, string>)) expect(en[k] !== undefined || en[`${k}.other`] !== undefined, k).toBe(true);
  });
});
