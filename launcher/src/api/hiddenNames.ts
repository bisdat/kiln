// The game's old, test and unused entries by the markers in their names: kiln_manage::catalog::is_hidden_name, for the
// browser mock's pickers and lint (the launcher itself gets the backend's `hidden` flag). Tested in unit/hidden-names.test.ts
// against the Rust test's names. Entries the game's own data uses stay listed (catalog::used_names); the mock knows one:
// station tank water.
const WORDS = new Set(["deprecated", "donotplace", "donotuse", "unused", "notfinished", "norecipe", "wip", "debug", "cheat", "prototype", "proto", "placeholder"]);

/** The game's "Water" item: station tank water in native recipes, never hidden (catalog::STATION_WATER). */
export const STATION_WATER = "Food_T1_raw_hydration_Water_Pouch_Filled_DEPRECATED";

export function isHiddenGameName(name: string): boolean {
  const lower = name.toLowerCase();
  if (lower.startsWith("z_") || ["do_not_place", "do_not_use", "to_be_deleted"].some((p) => lower.includes(p))) return true;
  return lower.split("_").filter(Boolean).some((w, i) => WORDS.has(w) || (w.startsWith("test") && (/^\d*$/.test(w.slice(4)) || i === 0)) || (i > 0 && w === "old"));
}
