// The game's old, test and unused entries by the markers in their names: kiln_manage::catalog::is_hidden_name, for the
// browser mock's pickers and lint (the launcher itself gets the backend's `hidden` flag). Tested in unit/hidden-names.test.ts
// against the Rust test's names. Entries the game's own data uses stay listed (catalog::used_names). The game's old Water
// canteen item (stationWater.ts OLD_WATER_ITEM) is hidden like the rest since 0.2.0-dev.30.3: it is not a station's tank.
const WORDS = new Set(["deprecated", "donotplace", "donotuse", "unused", "notfinished", "norecipe", "wip", "debug", "cheat", "prototype", "proto", "placeholder"]);

export function isHiddenGameName(name: string): boolean {
  const lower = name.toLowerCase();
  if (lower.startsWith("z_") || ["do_not_place", "do_not_use", "to_be_deleted"].some((p) => lower.includes(p))) return true;
  return lower.split("_").filter(Boolean).some((w, i) => WORDS.has(w) || (w.startsWith("test") && (/^\d*$/.test(w.slice(4)) || i === 0)) || (i > 0 && w === "old"));
}
