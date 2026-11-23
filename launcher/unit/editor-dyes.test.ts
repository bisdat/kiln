// The save editor adds dyes (Wulfhram, 2026-11-22): ColorPalette items are ordinary backpack stacks, so the catalog
// lists them (with a Dyes filter); Customization entries are unlocks the game keeps elsewhere and stay out.
import { expect, test } from "bun:test";
import { allItems, filterItems } from "../src/editor/kiln/Catalog.jsx";
import { items } from "../src/editor/src/gamedata.js";

const byCategory = (c: string) => Object.entries(items as Record<string, { category: string }>).filter(([, it]) => it.category === c).map(([id]) => Number(id));

test("dyes are in the catalog and under Dyes; customization entries are not", () => {
  const all = allItems() as { id: number; category: string }[];
  const ids = new Set(all.map((i) => i.id));
  // Shades Of Red and Snowfall, from the report
  expect(ids.has(835723769)).toBe(true);
  expect(ids.has(468594079)).toBe(true);
  const dyes = filterItems(all, { cat: "dyes" }) as { category: string }[];
  expect(dyes.length).toBe(byCategory("ColorPalette").filter((id) => ids.has(id)).length);
  expect(dyes.length).toBeGreaterThan(50);
  expect(dyes.every((d) => d.category === "ColorPalette")).toBe(true);
  for (const id of byCategory("Customization")) expect(ids.has(id)).toBe(false);
});
