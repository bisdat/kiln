// Pack Studio's "Any of" recipe input (src/studio/data/categories.ts, model.ts): category stacks read and written back
// exactly ({category, count}, a rule's numeric id kept as a number), and the category picker's words and search.
import { expect, test } from "bun:test";
import { filterCategories, findCategory, idNumber, onlyCategoryOf, sameId, summary, type Category } from "../src/studio/data/categories";
import { isCategoryStack, readDoc, recipeDefValue, recipeDefsOf, recipeRuleBody, recipeRulesOf, stacksOf, stacksValue, writeDoc } from "../src/studio/data/model";

const WOOD: Category = { id: "0xee5ae93f", items: [
  { name: "Material_T1_Wood_Logs", displayName: "Wood Logs" }, { name: "Material_T2_Wood_Hardwood", displayName: "Hardwood" },
  { name: "Material_T4_Wood_Logs_Palm", displayName: "Palm Wood Logs" }, { name: "Material_T5_Wood_Logs_Pine", displayName: "Conifer Logs" },
  { name: "Material_T6_Wood_Logs_Bamboo", displayName: "Bamboo Logs" }, { name: "Material_T6_Wood_Logs_Tropical" },
], recipes: [{ name: "Recipe_Tool_Camera", displayName: "Camera" }] };
const STONE: Category = { id: "0xb053f0f1", items: [{ name: "Material_T1_Stone", displayName: "Stone" }, { name: "Material_T1_Stone_Grey", displayName: "Stone" },
  { name: "Material_T3_Limestone", displayName: "Limestone" }], recipes: [] };
const FISH_A: Category = { id: "0x1d9e5177", items: [{ name: "Fish_Silverback", displayName: "Silverback" }, { name: "Fish_Crimsonback", displayName: "Crimsonback" }], recipes: [] };
const FISH_B: Category = { id: "0x5b622eb9", items: [{ name: "Fish_Silverback", displayName: "Silverback" }, { name: "Fish_Lakehopper", displayName: "Lakehopper" }], recipes: [] };
const OLD: Category = { id: "0x00000077", items: [{ name: "Z_Material_Sand_DEPRECATED", displayName: "Sand", hidden: true }], recipes: [] };
const ALL = [WOOD, STONE, FISH_A, FISH_B, OLD];

test("a recipe file with category inputs comes back byte for byte (new recipes and changes)", () => {
  const text = writeDoc({ format: 1, recipes: {
    add: [{ guid: "6f1c1c4e-3c0a-4f57-9d55-0f2f3a7c1d11", name: "Coal_From_Any_Wood", station: "0x0c0c0001",
      inputs: [{ category: "0xee5ae93f", count: 2 }, { item: "Material_T1_Stone", count: 1 }], outputs: [{ item: "Material_Charcoal", count: 1 }], duration: 5 }],
    Recipe_Tool_Camera: { inputs: [{ item: "Material_T1_Metal_Scraps", count: 1 }, { category: 3998935359, count: 2 }, { category: "0xB053F0F1", count: 3 }], craftingDuration: 4 },
  } });
  const s = readDoc(text).value.recipes;
  const defs = recipeDefsOf(s), rules = recipeRulesOf(s);
  expect(defs[0].inputs[0]).toEqual({ item: "", count: 2, category: "0xee5ae93f" });
  expect(isCategoryStack(defs[0].inputs[0])).toBe(true);
  expect(isCategoryStack(defs[0].inputs[1])).toBe(false);
  expect(rules[0].inputs?.[1]).toEqual({ item: "", count: 2, category: 3998935359 });
  const body = { add: defs.map(recipeDefValue), ...Object.fromEntries(rules.map((r) => [r.sel, recipeRuleBody(r)])) };
  expect(writeDoc({ format: 1, recipes: body })).toBe(text);
});

test("stacksValue: {category, count}; a category not chosen yet is left out; a stack holding both keeps both", () => {
  expect(stacksValue([{ item: "", count: 3, category: "0xee5ae93f" }, { item: "", count: 1, category: "" }, { item: "  ", count: 1 }, { item: "Material_T1_Stone ", count: 2 }]))
    .toEqual([{ category: "0xee5ae93f", count: 3 }, { item: "Material_T1_Stone", count: 2 }]);
  expect(stacksValue(stacksOf([{ item: "Material_T1_Stone", category: "0xb053f0f1", count: 1 }])!)).toEqual([{ item: "Material_T1_Stone", category: "0xb053f0f1", count: 1 }]);
  expect(isCategoryStack({ item: "", count: 1, category: "" })).toBe(true);
});

test("ids compare as numbers", () => {
  expect(idNumber("0xEE5AE93F")).toBe(3998935359);
  expect(idNumber(3998935359)).toBe(3998935359);
  expect(idNumber("ee5ae93f")).toBe(3998935359);
  expect(idNumber("not an id")).toBeNull();
  expect(sameId("0x0d0d0001", "0x0D0D0001")).toBe(true);
  expect(sameId("0x0d0d0001", "0x0d0d0002")).toBe(false);
  expect(findCategory(ALL, 3998935359)).toBe(WOOD);
  expect(findCategory(ALL, "0x12345678")).toBeUndefined();
});

test("a category in words: its first distinct item names (display names, else names) and how many more", () => {
  expect(summary(WOOD, false)).toEqual({ names: ["Wood Logs", "Hardwood", "Palm Wood Logs"], more: 3,
    all: ["Wood Logs", "Hardwood", "Palm Wood Logs", "Conifer Logs", "Bamboo Logs", "Material_T6_Wood_Logs_Tropical"] });
  // two items both shown as "Stone" are one name
  expect(summary(STONE, false)).toEqual({ names: ["Stone", "Limestone"], more: 0, all: ["Stone", "Limestone"] });
});

test("the picker finds a category by any of its items (display name or name, every word), by its id, and ranks exact words first", () => {
  expect(filterCategories(ALL, "", false)).toEqual([WOOD, STONE, FISH_A, FISH_B]);
  expect(filterCategories(ALL, "pine", false)).toEqual([WOOD]);
  expect(filterCategories(ALL, "conifer", false)).toEqual([WOOD]);
  expect(filterCategories(ALL, "palm logs", false)).toEqual([WOOD]);
  expect(filterCategories(ALL, "palm stone", false)).toEqual([]);
  expect(filterCategories(ALL, "0xB053F0F1", false)).toEqual([STONE]);
  expect(filterCategories(ALL, "silverback", false)).toEqual([FISH_A, FISH_B]);
  // "stone" is a whole name in the stones, only part of a word elsewhere
  const withStoneWood: Category = { id: "0x00000099", items: [{ name: "Material_Stonewood", displayName: "Stonewood" }], recipes: [] };
  expect(filterCategories([withStoneWood, STONE], "stone", false)).toEqual([STONE, withStoneWood]);
});

test("hidden game entries inside categories follow Show hidden game entries", () => {
  expect(filterCategories(ALL, "sand", false)).toEqual([]);
  expect(filterCategories(ALL, "sand", true)).toEqual([OLD]);
  expect(summary(OLD, false).names).toEqual([]);
  expect(summary(OLD, true).names).toEqual(["Sand"]);
});

test("an item only one category holds offers it; one in two categories (or none) offers nothing", () => {
  expect(onlyCategoryOf(ALL, "Material_T5_Wood_Logs_Pine", false)).toBe(WOOD);
  expect(onlyCategoryOf(ALL, "Fish_Silverback", false)).toBeNull();
  expect(onlyCategoryOf(ALL, "Fish_Lakehopper", false)).toBe(FISH_B);
  expect(onlyCategoryOf(ALL, "Material_T1_Iron_Bar", false)).toBeNull();
  expect(onlyCategoryOf(ALL, "", false)).toBeNull();
  expect(onlyCategoryOf(ALL, "Z_Material_Sand_DEPRECATED", false)).toBeNull();
  expect(onlyCategoryOf(ALL, "Z_Material_Sand_DEPRECATED", true)).toBe(OLD);
});
