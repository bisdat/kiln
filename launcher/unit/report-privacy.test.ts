import { expect, test } from "bun:test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { NEVER } from "../src/api/mockReport";

const locales = join(import.meta.dir, "..", "src", "i18n");
const exceptionWords: Record<string, readonly string[]> = {
  de: ["Manifest-IDs", "-Versionen"],
  en: ["manifest IDs", "versions"],
  es: ["identificadores", "versiones", "manifiestos"],
  fr: ["identifiants", "versions", "manifestes"],
  it: ["ID", "versioni", "manifest"],
  ja: ["マニフェストID", "バージョン"],
  ko: ["매니페스트 ID", "버전"],
  pl: ["identyfikatory", "wersje", "manifestów"],
  "pt-BR": ["IDs", "versões", "manifestos"],
  ru: ["идентификаторы", "версии", "манифестов"],
  "zh-CN": ["清单 ID", "版本"],
};

test("every shipped locale has a checked support-report privacy notice", () => {
  expect(readdirSync(locales).filter((f) => f.endsWith(".json")).map((f) => f.slice(0, -5)).sort())
    .toEqual(Object.keys(exceptionWords).sort());
});

for (const [tag, words] of Object.entries(exceptionWords)) {
  test(`${tag}: the mod-content exception names only ShroudForge manifest IDs and versions`, () => {
    const notice = JSON.parse(readFileSync(join(locales, `${tag}.json`), "utf8")).reportContents.never.mods;
    expect(notice).toContain("ShroudForge");
    for (const word of words) expect(notice).toContain(word);
  });
}

test("the browser preview's mod-content notice matches the backend", () => {
  const source = readFileSync(join(import.meta.dir, "..", "..", "crates", "kiln-manage", "src", "report.rs"), "utf8");
  const backend = source.match(/\("mods", "([^"]+)"\)/)?.[1];
  expect(backend).toBeDefined();
  expect(NEVER.find((item) => item.id === "mods")?.text).toBe(backend);
});
