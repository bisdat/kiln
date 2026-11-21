// A recipe's water in Pack Studio (src/api/stationWater.ts, src/studio/data/model.ts): the station tables agree with
// kiln_manage::catalog's (TANK_STATIONS, DRY_STATIONS), "water" / "waterCharges" read and written back, and the old Water
// canteen item becomes tank water only by the creator's own choice (toTankWater).
import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { DRY_STATIONS, OLD_WATER_ITEM, TANK_STATIONS, WATER_MAX, stationWater } from "../src/api/stationWater";
import { isHiddenGameName } from "../src/api/hiddenNames";
import { oldWaterCount, readDoc, recipeDefValue, recipeDefsOf, recipeRuleBody, recipeRulesOf, toTankWater, writeDoc } from "../src/studio/data/model";

const rust = readFileSync(join(import.meta.dir, "../../crates/kiln-manage/src/catalog/mod.rs"), "utf8");
const block = (name: string) => rust.slice(rust.indexOf("pub const " + name), rust.indexOf("];", rust.indexOf("pub const " + name)));

test("the station tables are kiln-manage's", () => {
  const tanks = [...block("TANK_STATIONS").matchAll(/\((0x[0-9a-f]{8}), (\d+), "([^"]+)"\)/g)].map((m) => [parseInt(m[1], 16), Number(m[2]), m[3]]);
  const dry = [...block("DRY_STATIONS").matchAll(/\((0x[0-9a-f]{8}), "([^"]+)"\)/g)].map((m) => [parseInt(m[1], 16), m[2]]);
  expect(tanks.length).toBe(6);
  expect(dry.length).toBeGreaterThan(20);
  expect(TANK_STATIONS.map((x) => [...x])).toEqual(tanks);
  expect(DRY_STATIONS.map((x) => [...x])).toEqual(dry);
  expect(rust).toContain('pub const STATION_WATER: &str = "' + OLD_WATER_ITEM + '"');
  expect(readFileSync(join(import.meta.dir, "../../native/services/content/recipe_water.h"), "utf8")).toContain("kMax = " + WATER_MAX + ";");
});

test("what a station does with a recipe's water", () => {
  expect(stationWater("0xba67134f")).toEqual({ kind: "tank", charges: 150, name: "Seedbed" });
  expect(stationWater("0xBA67134F")).toEqual({ kind: "tank", charges: 150, name: "Seedbed" });
  expect(stationWater("0xf5aab50a")).toEqual({ kind: "noTank", name: "Smelter" });
  expect(stationWater("hand")).toEqual({ kind: "menu" });
  expect(stationWater("0x4da88f73")).toEqual({ kind: "menu" }); // the Workbench
  expect(stationWater("Recipe_Bread")).toEqual({ kind: "unknown" });
  expect(stationWater("workshop:6f1c1c4e-3c0a-4f57-9d55-0f2f3a7c1d11")).toEqual({ kind: "unknown" });
});

test("water and waterCharges come back as written; other keys keep their place", () => {
  const doc = { format: 1, recipes: {
    add: [
      { guid: "6f1c1c4e-3c0a-4f57-9d55-0f2f3a7c1d11", name: "Tea", station: "0x6b8247d6", inputs: [{ item: "Herb", count: 2 }], outputs: [{ item: "Tea", count: 1 }], duration: 30, water: 5 },
      { guid: "6f1c1c4e-3c0a-4f57-9d55-0f2f3a7c1d12", name: "Plain", station: "hand", inputs: [{ item: "Herb", count: 1 }], outputs: [{ item: "Tea", count: 1 }] },
    ],
    Recipe_Soup: { craftingDuration: 4, waterCharges: 2 },
    Recipe_Stew: { craftingDuration: 4 },
  } };
  const text = writeDoc(doc);
  const s = readDoc(text).value.recipes;
  const defs = recipeDefsOf(s), rules = recipeRulesOf(s);
  expect(defs[0].water).toBe(5);
  expect(defs[0].rest).toEqual({});
  expect(defs[1].water).toBeNull();
  expect(rules[0].water).toBe(2);
  expect(rules[1].water).toBeNull();
  const back = writeDoc({ format: 1, recipes: { add: defs.map(recipeDefValue), ...Object.fromEntries(rules.map((r) => [r.sel, recipeRuleBody(r)])) } });
  expect(back).toBe(text);
  // a water the editor cannot read stays as written, for the data check to name
  const odd = recipeDefsOf({ add: [{ guid: "g", name: "Odd", station: "hand", inputs: [], outputs: [], water: "lots" }] });
  expect(odd[0].water).toBeNull();
  expect(recipeDefValue(odd[0]).water).toBe("lots");
});

test("the old Water item: counted, and turned into tank water only on request", () => {
  expect(isHiddenGameName(OLD_WATER_ITEM)).toBe(true);
  const def = recipeDefsOf({ add: [{ guid: "g", name: "Tea", station: "0x6b8247d6",
    inputs: [{ item: "Herb", count: 2 }, { item: OLD_WATER_ITEM, count: 2 }, { item: OLD_WATER_ITEM, count: 1 }], outputs: [{ item: "Tea", count: 1 }] }] })[0];
  expect(oldWaterCount(def.inputs, OLD_WATER_ITEM)).toBe(3);
  // reading it changes nothing: the canteen stays an input until the creator chooses
  expect(recipeDefValue(def).inputs).toEqual([{ item: "Herb", count: 2 }, { item: OLD_WATER_ITEM, count: 2 }, { item: OLD_WATER_ITEM, count: 1 }]);
  expect(recipeDefValue(def).water).toBeUndefined();
  const tank = toTankWater(def, OLD_WATER_ITEM);
  expect(tank.inputs).toEqual([{ item: "Herb", count: 2 }]);
  expect(tank.water).toBe(3);
  expect(recipeDefValue(tank)).toMatchObject({ inputs: [{ item: "Herb", count: 2 }], water: 3 });
  // on top of water the recipe already asks; a rule writes waterCharges
  const rule = recipeRulesOf({ Recipe_Soup: { inputs: [{ item: OLD_WATER_ITEM, count: 1 }], waterCharges: 2 } })[0];
  expect(recipeRuleBody(toTankWater(rule, OLD_WATER_ITEM))).toEqual({ inputs: [], waterCharges: 3 });
  // nothing to do: the same object back
  const plain = recipeDefsOf({ add: [{ guid: "g", name: "P", station: "hand", inputs: [{ item: "Herb", count: 1 }], outputs: [] }] })[0];
  expect(toTankWater(plain, OLD_WATER_ITEM)).toBe(plain);
});
