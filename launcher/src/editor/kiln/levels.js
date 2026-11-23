// Save item levels, rarity and applied upgrades are bytes (shared/character.mjs). The saved level includes upgrades.
// The vendored catalog may lag the game or mods, so its maxLevel is not an edit limit. See UPSTREAM.md.
export const MAX_ITEM_LEVEL = 255;
const byte = (v) => Math.max(0, Math.min(MAX_ITEM_LEVEL, Math.floor(Number(v)) || 0));

// Use the instance's own upgrade capacity, not the catalog's rarity or a fixed five-upgrade assumption.
export const upgradeLimit = (entry) => byte(entry.maxUpgrades ?? MAX_ITEM_LEVEL);
const applied = (entry) => Math.min(upgradeLimit(entry), byte(entry.upgrades ?? 0));

/** Only called for an edit, never while reading or displaying a save. */
export function clampItemLevel(entry, value) {
  return Math.max(applied(entry), byte(value));
}

/** Changing upgrades changes the total level too; rarity is changed only when the player chose a rarity. */
export function setItemUpgrades(entry, value, rarity) {
  const next = { ...entry };
  if (rarity !== undefined) next.maxUpgrades = byte(rarity);
  next.upgrades = Math.min(upgradeLimit(next), byte(value));
  if (entry.level !== undefined) {
    const base = Math.max(0, entry.level - (entry.upgrades ?? 0));
    next.level = clampItemLevel(next, base + next.upgrades);
  }
  return next;
}

/** A template creates a new item; unlike an existing save's entry, its values are bounded now. */
export function newItemLevels(entry) {
  const next = entry.maxUpgrades !== undefined || entry.upgrades !== undefined
    ? setItemUpgrades(entry, entry.upgrades ?? 0, entry.maxUpgrades) : { ...entry };
  if (next.level !== undefined) next.level = clampItemLevel(next, next.level);
  return next;
}

/** Write validation is deliberately byte-only: never reject a game-written value for exceeding catalog limits. */
export const itemLevelsFit = (entry) => ['level', 'maxUpgrades', 'upgrades'].every((key) =>
  entry[key] === undefined || (Number.isInteger(entry[key]) && entry[key] >= 0 && entry[key] <= MAX_ITEM_LEVEL));
