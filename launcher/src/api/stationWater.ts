// A recipe's water (RecipeInfo.waterCharges: a new recipe's "water", a change's "waterCharges"; charges a craft, 64 water
// units each) and the stations that can give it: kiln_manage::catalog::TANK_STATIONS / DRY_STATIONS, checked against the
// Rust tables by unit/station-water.test.ts. A factory station takes a recipe's water from its own tank only (a station
// without one never starts such a recipe); a crafting menu (hand, Workbench, Kitchen, NPCs) from the crafter and the
// base's wells and barrels (docs/re/stations.md section 3.6). kiln lint KL408 says the same in the data check.

/** The game's old "Water" item, a leather canteen (catalog::STATION_WATER). The recipe screen borrows its name and icon
 *  for a recipe's water row, but as a recipe input it is that item, never the station's tank. */
export const OLD_WATER_ITEM = "Food_T1_raw_hydration_Water_Pouch_Filled_DEPRECATED";

/** The most water a recipe may ask (native recipe_water.h, schema "water"). */
export const WATER_MAX = 1023;

/** Factory stations with a tank: workshop id, tank charges, name. */
export const TANK_STATIONS: readonly (readonly [number, number, string])[] = [
  [0xba67134f, 150, "Seedbed"],
  [0x1ee9f0bf, 100, "Oven"],
  [0x39fbd98f, 100, "Cooking Station"],
  [0x20b86c20, 100, "Laboratory"],
  [0x6b8247d6, 100, "Coffee Brewer"],
  [0x269a27a7, 50, "Beehive"],
];

/** Factory stations without a tank: workshop id, name. */
export const DRY_STATIONS: readonly (readonly [number, string])[] = [
  [0x6ad05fce, "Butter Churn"], [0xd08b9300, "Alchemy Station"], [0x485ec29c, "Blast Furnace"], [0x34564abc, "Force Generator"],
  [0xa6ebcde3, "Roaster"], [0x8dca2c49, "Forge"], [0x865085ea, "Infusion Altar"], [0xe26134b5, "Spinning Wheel"],
  [0x3a3cc728, "Loom"], [0x7d96fde8, "Seafood Grill"], [0xa4b9eaa5, "Tanning Station"], [0x847eab53, "Kiln"],
  [0x2546ae49, "Charcoal Kiln"], [0x1abf2d27, "Ectoplasm Press"], [0xf5aab50a, "Smelter"], [0xd4796c9a, "Spinning Machine"],
  [0x73306967, "Hand Spindle"], [0x0a921c7b, "Farmbed"], [0x056d94f8, "Grinding Stones"], [0x3b51fb95, "Coloring Station"],
  [0x1abf8143, "Table Saw"], [0x6b95dc33, "Drying Rack"], [0x7251e0d8, "Mill"],
];

export type StationWater = { kind: "tank"; charges: number; name: string } | { kind: "noTank"; name: string } | { kind: "menu" } | { kind: "unknown" };

/** What a new recipe's station ("hand", "0x<workshop id>", a recipe name, "workshop:<guid>") does with its water. Only
 *  "hand" and a workshop id are known here; the data check (KL408) follows a recipe name through the catalog. */
export function stationWater(station: string): StationWater {
  const s = station.trim();
  if (/^(hand|player)$/i.test(s)) return { kind: "menu" };
  if (!/^0x[0-9a-f]{1,8}$/i.test(s)) return { kind: "unknown" };
  const id = parseInt(s.slice(2), 16);
  const tank = TANK_STATIONS.find((x) => x[0] === id);
  if (tank) return { kind: "tank", charges: tank[1], name: tank[2] };
  const dry = DRY_STATIONS.find((x) => x[0] === id);
  return dry ? { kind: "noTank", name: dry[1] } : { kind: "menu" };
}
