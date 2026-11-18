import { expect, test } from "bun:test";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { flatten } from "../src/i18n/format";
import { checkLocale, hasKey, pluralShapeProblems, readLocaleFile, sourceKeys, untranslated } from "./i18n-check";

const i18n = join(import.meta.dir, "..", "src", "i18n");
const en = flatten(JSON.parse(readFileSync(join(i18n, "en.json"), "utf8")));

test("a translation with unknown keys, non-text values and changed placeholders is flagged", () => {
  const enT = flatten({ a: "Hello {name}", n: { one: "{count} mod", other: "{count} mods" }, p: "{0} of {1}" });
  const problems = checkLocale(enT, {
    a: "你好 {nmae}",
    n: { other: "{count} 个模组", one: "一个模组" },
    p: "{1} / {0}",
    typo: "x",
    nested: { bad: 3 },
  });
  expect(problems.map((p) => `${p.key} ${p.kind}`).sort()).toEqual([
    "a placeholders", "nested.bad not-text", "typo unknown-key",
  ]);
  expect(problems.find((p) => p.key === "a")!.message).toBe("missing {name}; not in English: {nmae}");
  expect(checkLocale(enT, [])[0].kind).toBe("not-json");
  expect(untranslated(enT, { n: { other: "x" } })).toEqual(["a", "p"]);
});

test("a translation is data: markup, links, control characters, other number formats and bad confirm words are flagged", () => {
  const enT = flatten({
    a: "Read {file}", n: { one: "{count:N0} icon", other: "{count:N0} icons" }, site: "See https://frostdev.io/kiln.",
    lines: "One\nTwo", common: { confirmWord: { delete: "delete", clean: "clean" } }, s: "Short",
  });
  const problems = checkLocale(enT, {
    a: "读取 <b>{file}</b>&nbsp;[这里](https://evil.example)",
    n: { other: "{count} 个图标" },
    site: "见 https://frostdev.io/kiln。",
    lines: "一\n二‮",
    common: { confirmWord: { delete: "删 除", clean: "清理" } },
    s: "",
  });
  expect(problems.map((p) => `${p.key} ${p.kind}`).sort()).toEqual([
    "a markup", "a markup", "a markup", "a markup", "a markup",
    "common.confirmWord.delete confirm-word", "lines control", "n.other format", "s empty",
  ]);
  expect(problems.filter((p) => p.key === "a").map((p) => p.message)).toContain("the link https://evil.example, which English has not here");
  expect(problems.find((p) => p.key === "lines")!.message).toContain("U+202E");
  expect(checkLocale(enT, { s: "短".repeat(200) }).map((p) => p.kind)).toEqual(["length"]);
});

test("en.json: every value is text and every t(\"key\") in the source is in it", () => {
  const bad: string[] = [];
  flatten(JSON.parse(readFileSync(join(i18n, "en.json"), "utf8")), "", {}, bad);
  expect(bad).toEqual([]);
  const missing = sourceKeys(join(import.meta.dir, "..", "src")).filter((k) => !hasKey(en, k.key)).map((k) => `${k.file}:${k.line} ${k.key}`);
  expect(missing).toEqual([]);
  expect(pluralShapeProblems(en)).toEqual([]);
  expect(pluralShapeProblems(flatten({ type: { kiln: "Kiln mod", other: "Not a Kiln mod" }, n: { one: "x" } }))).toHaveLength(2);
});

test("every locale file beside en.json passes the check", () => {
  for (const f of readdirSync(i18n).filter((x) => x.endsWith(".json") && x !== "en.json")) {
    const { value, error } = readLocaleFile(join(i18n, f));
    expect(error).toBeNull();
    expect(checkLocale(en, value).map((p) => `${f} ${p.key}: ${p.message}`)).toEqual([]);
  }
});
