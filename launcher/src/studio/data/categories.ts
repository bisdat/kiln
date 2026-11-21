// The recipe editor's "Any of" ingredient: the game's recipe input categories (studio_categories; catalog itemCategories),
// worded by their items as the item pickers show them, searchable by any member item. A recipe input {"category": id}
// takes any item of the category (docs/kiln/services/stations.md, "Item categories"). Pure: unit-tested in
// unit/studio-categories.test.ts.
import type { StudioItemCategory } from "../../api/commands";

export type Category = StudioItemCategory;
export type CategoryItem = Category["items"][number];

/** An item's words as the pickers show them: its display name, else its name. */
export const itemLabel = (i: { name: string; displayName?: string }): string => i.displayName || i.name;

/** The members shown: the game's old, test and unused entries only with "Show hidden game entries" (as the pickers). */
export const shownItems = (c: Category, showHidden: boolean): CategoryItem[] => c.items.filter((i) => showHidden || !i.hidden);

/** A category id as a number ("0x0D0D0001", "0x0d0d0001" and 218955777 are one), or null. */
export function idNumber(id: string | number | undefined): number | null {
  if (typeof id === "number") return Number.isInteger(id) && id >= 0 ? id : null;
  const s = (id ?? "").trim();
  if (/^0[xX][0-9a-fA-F]{1,8}$/.test(s)) return parseInt(s.slice(2), 16);
  if (/^\d+$/.test(s)) return Number(s);
  // definition stacks may write the id without 0x (the schema reads it as hex)
  if (/^[0-9a-fA-F]{1,8}$/.test(s)) return parseInt(s, 16);
  return null;
}
export const sameId = (a: string | number | undefined, b: string | number | undefined): boolean => {
  const x = idNumber(a);
  return x !== null && x === idNumber(b);
};
export const findCategory = (list: readonly Category[], id: string | number | undefined): Category | undefined => list.find((c) => sameId(c.id, id));

/** The category in words: its first distinct item names and how many more ("Wood Logs, Hardwood, Palm Wood Logs" +3). */
export function summary(c: Category, showHidden: boolean, max = 3): { names: string[]; more: number; all: string[] } {
  const all = [...new Set(shownItems(c, showHidden).map(itemLabel))];
  return { names: all.slice(0, max), more: Math.max(0, all.length - max), all };
}

/** The picker's hits for a query: every word in one member's display name or name, or the category's id (or name); a
 *  category with no member shown is left out. Exact member words first, then a member starting with the query. */
export function filterCategories(list: readonly Category[], query: string, showHidden: boolean): Category[] {
  const q = query.trim().toLowerCase();
  const words = q.split(/\s+/).filter(Boolean);
  const ranked: [number, Category][] = [];
  for (const c of list) {
    const items = shownItems(c, showHidden);
    if (!items.length) continue;
    if (!words.length) { ranked.push([2, c]); continue; }
    if (sameId(c.id, q) || (!!c.name && c.name.toLowerCase().includes(q))) { ranked.push([0, c]); continue; }
    let best = 3;
    for (const i of items) {
      const fields = [itemLabel(i).toLowerCase(), i.name.toLowerCase()];
      if (!words.every((w) => fields.some((f) => f.includes(w)))) continue;
      best = Math.min(best, fields.includes(q) ? 0 : fields.some((f) => f.startsWith(q)) ? 1 : 2);
    }
    if (best < 3) ranked.push([best, c]);
  }
  return ranked.sort((a, b) => a[0] - b[0]).map(([, c]) => c);
}

/** The one category that holds this item (the "any of these instead" offer); none when no category or several do. */
export function onlyCategoryOf(list: readonly Category[], item: string, showHidden: boolean): Category | null {
  const n = item.trim();
  if (!n) return null;
  const holding = list.filter((c) => shownItems(c, showHidden).some((i) => i.name === n));
  return holding.length === 1 ? holding[0] : null;
}
