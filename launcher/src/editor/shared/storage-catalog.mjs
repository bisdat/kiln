// SPDX-License-Identifier: MIT
// Loaded with the editor API, BEFORE gameart merges mod overlays into this shared JSON object.
// Keep an immutable base-game lookup: a cached mod item must never be returned to a vanilla save.
import items from '../data/items.json';
const vanillaLimits = new Map(Object.entries(items).map(([id, item]) => [Number(id), item.maxStack]));
export const vanillaStackLimit = (id) => vanillaLimits.get(id) ?? null;
