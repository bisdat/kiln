// Save item levels, rarity and applied upgrades are bytes (shared/character.mjs). For a known vanilla item,
// items.json bounds the base level; the saved level includes applied upgrades, capped by instance rarity (1..5).
// See UPSTREAM.md. Incomplete data and mod items use only the byte bound: their game limits are not known here.
export const MAX_ITEM_LEVEL = 255;
const RARITIES = new Set(['Common', 'Uncommon', 'Rare', 'Epic', 'Legendary']);
const byte = (v) => Math.max(0, Math.min(MAX_ITEM_LEVEL, Math.floor(Number(v)) || 0));
const knownLimits = (item) => !item?.modded && Number.isInteger(item?.maxLevel) && item.maxLevel > 0 && RARITIES.has(item.rarity);

export const baseLevelLimit = (item) => knownLimits(item) ? Math.min(MAX_ITEM_LEVEL, item.maxLevel) : MAX_ITEM_LEVEL;
export const upgradeLimit = (entry, item) => knownLimits(item) ? Math.min(5, byte(entry.maxUpgrades ?? 0)) : MAX_ITEM_LEVEL;
const applied = (entry, item) => Math.min(upgradeLimit(entry, item), byte(entry.upgrades ?? 0));
export const itemLevelLimit = (entry, item) => Math.min(MAX_ITEM_LEVEL, baseLevelLimit(item) + applied(entry, item));

/** Only called for an edit, never while reading or displaying a save. */
export function clampItemLevel(entry, item, value) {
  return Math.max(applied(entry, item), Math.min(itemLevelLimit(entry, item), byte(value)));
}

/** Changing upgrades changes the total level too; rarity is changed only when the player chose a rarity. */
export function setItemUpgrades(entry, item, value, rarity) {
  const next = { ...entry };
  if (rarity !== undefined) next.maxUpgrades = Math.min(knownLimits(item) ? 5 : MAX_ITEM_LEVEL, byte(rarity));
  next.upgrades = Math.min(upgradeLimit(next, item), byte(value));
  if (entry.level !== undefined) {
    const base = Math.max(0, entry.level - (entry.upgrades ?? 0));
    next.level = clampItemLevel(next, item, base + next.upgrades);
  }
  return next;
}

/** A template creates a new item; unlike an existing save's entry, its values are bounded now. */
export function newItemLevels(entry, item) {
  const next = entry.maxUpgrades !== undefined || entry.upgrades !== undefined
    ? setItemUpgrades(entry, item, entry.upgrades ?? 0, entry.maxUpgrades) : { ...entry };
  if (next.level !== undefined) next.level = clampItemLevel(next, item, next.level);
  return next;
}

/** Write validation is deliberately byte-only: never reject a game-written value for exceeding catalog limits. */
export const itemLevelsFit = (entry) => ['level', 'maxUpgrades', 'upgrades'].every((key) =>
  entry[key] === undefined || (Number.isInteger(entry[key]) && entry[key] >= 0 && entry[key] <= MAX_ITEM_LEVEL));
