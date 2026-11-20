// The inventory header search in the save editor's character parser (src/editor/shared/character.mjs). The header
// before the inventory is four bytes, 05 <main count> <groups> <extra count>, and a header with no entries reads as soon
// as it is found: the bytes 05 00 00 00 (a u32 5, common in a save) match it. Player report 2026-11-20: the Backpack tab
// was empty while Equipment worked. Blocks here are built in the test, never read from a player's save.
import { describe, expect, test } from "bun:test";
import { Buffer } from "buffer";
// @ts-ignore: the editor is plain JavaScript
import * as ch from "../src/editor/shared/character.mjs";

const { parseCharacter, buildCharacter } = ch;
import items from "../src/editor/data/items.json";
import skills from "../src/editor/data/skills.json";

const isItem = (id: number) => Object.hasOwn(items, id);
const isSkill = (id: number) => Object.hasOwn(skills.nodes, id);
const WORN = new Set(["Armour_Head", "Armour_UpperBody", "Armour_Arms", "Armour_LowerBody", "Armour_Feet", "Armour_Ring", "Shield", "HookShot", "Glider", "Backpack"]);
const wearable = (id: number) => WORN.has((items as Record<string, { slot?: string }>)[id]?.slot ?? "");

interface E { slot: number; itemId: number; count: number; level?: number; newFlag?: boolean }
function entry(e: E): Buffer {
  const h = Buffer.alloc(8);
  h[0] = e.slot; h.writeUInt32LE(e.itemId >>> 0, 1); h.writeUInt16LE(e.count, 5);
  const parts = [h];
  let flags = 0;
  if (e.level !== undefined) { flags |= 0x01; parts.push(Buffer.from([e.level])); }
  if (e.newFlag) { flags |= 0x40; parts.push(Buffer.alloc(4)); } // a flag bit the parser does not know (a newer game)
  h[7] = flags;
  return Buffer.concat(parts);
}

const I = { helm: 63209182, shirt: 3372205, gloves: 27519013, pants: 9501187, boots: 30416182, glider: 284247111, backpack: 996296787,
  diamond: 61577244, scraps: 132545371, iron: 159114158, mana: 3087876 };
const UNKNOWN = 0x7f00abcd; // no table has it: a stack of a mod that is gone
const U32_FIVE = Buffer.from([5, 0, 0, 0]);

/** A player_character block: seven worn pieces (two weapon bits more in the mask), 20 free places, `between` bytes before
 *  the inventory header, then the inventory and `tail`. */
function block({ main, extra = [], between = Buffer.alloc(4), tail = Buffer.alloc(24), bagPages = 5, backpackPages = 4 }: { main: E[]; extra?: E[]; between?: Buffer; tail?: Buffer; bagPages?: number; backpackPages?: number }) {
  const top = Buffer.alloc(12);
  top.writeUInt32LE(25, 0); top.writeUInt32LE(100, 4); top.writeUInt32LE(1, 8);
  const node = Buffer.alloc(5); node.writeUInt32LE(31613866, 0); node[4] = 1;
  const worn: [number, number][] = [[5, I.helm], [6, I.shirt], [7, I.gloves], [8, I.pants], [9, I.boots], [14, I.glider], [15, I.backpack]];
  const mask = worn.reduce((m, [bit]) => m | (1 << bit), 0) | (1 << 16) | (1 << 17);
  const mid = Buffer.alloc(16); mid.writeUInt32LE(mask >>> 0, 12);
  return Buffer.concat([
    Buffer.alloc(0x3a), top, node, mid,
    ...worn.map(([, itemId], slot) => entry({ slot, itemId, count: 1, level: 10 })),
    Buffer.from([20, 0]), between, Buffer.from([bagPages, main.length, extra.length ? backpackPages : 0, extra.length]),
    ...main.map(entry), ...extra.map(entry), tail,
  ]);
}

const MAIN: E[] = [{ slot: 0, itemId: I.diamond, count: 120 }, { slot: 1, itemId: I.scraps, count: 5 }, { slot: 2, itemId: I.iron, count: 75 }];
const EXTRA: E[] = [{ slot: 0, itemId: I.mana, count: 3 }];
const strict = (p: Buffer) => parseCharacter(p, isItem, isSkill, wearable);
const read = (p: Buffer) => ch.readCharacter(p, isItem, isSkill, wearable);

describe("the inventory header", () => {
  test("a u32 5 between the equipment and the inventory is not an empty inventory", () => {
    const p = block({ main: MAIN, extra: EXTRA, between: Buffer.concat([Buffer.alloc(2), U32_FIVE, Buffer.alloc(2)]) });
    const c = strict(p);
    expect(c.equipment.length).toBe(7);
    expect(c.main.map((e: E) => e.itemId)).toEqual(MAIN.map((e) => e.itemId));
    expect(c.extra.length).toBe(1);
    expect(c._invUnsure).toBe(false);
    expect(Buffer.compare(buildCharacter(c), p)).toBe(0);
  });

  test("an unknown item in the backpack is read with unknown ids accepted, not as an empty inventory found further on", () => {
    const p = block({ main: [...MAIN, { slot: 3, itemId: UNKNOWN, count: 2 }], extra: EXTRA, tail: Buffer.concat([Buffer.alloc(10), U32_FIVE, Buffer.alloc(10)]) });
    const r = read(p);
    expect(r.unknownItems).toBe(true);
    expect(r.character.main.length).toBe(4);
    expect(r.character.main.some((e: E) => e.itemId === UNKNOWN)).toBe(true);
    expect(r.character.extra.length).toBe(1);
  });

  test("a save every reading finds empty while another header also fits is marked unsure, and items are not written into it", () => {
    // the entries carry a flag bit this parser does not know: neither reading can take the real header
    const p = block({ main: [...MAIN, { slot: 3, itemId: I.iron, count: 1, newFlag: true }], extra: EXTRA, tail: Buffer.concat([Buffer.alloc(10), U32_FIVE, Buffer.alloc(10)]) });
    const { character: c } = read(p);
    expect(c.main.length + c.extra.length).toBe(0);
    expect(c._invUnsure).toBe(true);
    // what was read writes back as it was
    expect(Buffer.compare(buildCharacter(c), p)).toBe(0);
    expect(() => buildCharacter({ ...c, main: [{ slot: 0, itemId: I.diamond, count: 1 }] })).toThrow();
  });

  test("a character with nothing in the inventory and one header is sure, and takes new items", () => {
    const p = block({ main: [] });
    const c = strict(p);
    expect(c.main.length + c.extra.length).toBe(0);
    expect(c._invUnsure).toBe(false);
    const added = buildCharacter({ ...c, main: [{ slot: 0, itemId: I.diamond, count: 7 }] });
    expect(strict(added).main).toEqual([{ slot: 0, itemId: I.diamond, count: 7, flags: 0 }]);
  });

  // Belt Lantern (AndyPimms' mod, docs/INVENTORY_STORAGE.md in its source): the worn lantern is a row of the bag (an id no
  // table has) plus a 96-byte LNTINV02 ... LNTEND02 record at the end of the character data; older builds of the mod
  // wrote a sixth bag page for it (slot 40). Player report 2026-11-20: the Backpack read empty and took new items.
  const LANTERN = 0x6c4e5401;
  const lanternRecord = () => Buffer.concat([Buffer.from("LNTINV02"), Buffer.alloc(80), Buffer.from("LNTEND02")]);

  test("Belt Lantern's lantern in a bag slot and its record: the inventory reads whole, read-only, and the mod is named", () => {
    const p = block({ main: [...MAIN, { slot: 7, itemId: LANTERN, count: 1 }], extra: EXTRA, tail: Buffer.concat([Buffer.alloc(10), U32_FIVE, Buffer.alloc(10), lanternRecord()]) });
    const r = read(p);
    expect(r.unknownItems).toBe(true);
    expect(r.character.main.map((e: E) => e.itemId)).toEqual([...MAIN.map((e) => e.itemId), LANTERN]);
    expect(ch.modDataIn(p)).toEqual(["Belt Lantern"]);
    expect(ch.modDataIn(block({ main: MAIN }))).toEqual([]);
  });

  test("an older Belt Lantern's sixth bag page (the lantern in slot 40) reads whole", () => {
    const p = block({ bagPages: 6, main: [...MAIN, { slot: 40, itemId: LANTERN, count: 1 }], extra: EXTRA, tail: Buffer.concat([Buffer.alloc(10), U32_FIVE, Buffer.alloc(10)]) });
    const r = read(p);
    expect(r.unknownItems).toBe(true);
    expect(r.character.main.length).toBe(4);
    expect(r.character.extra.length).toBe(1);
  });

  // the shape of the reporting player's save (game-written, 2026-11-20), rebuilt here with other items: 05 00 01 00 just
  // before the real header (the old search took it: empty Backpack), an item no table has (count 0x8001, a level) in
  // the bag, a 16-slot backpack (2 pages) holding a stack of count 0 (the game keeps such rows)
  test("the reported save's shape: the whole inventory reads, with the unknown item, and writes back the same bytes", () => {
    const main: E[] = [...MAIN, { slot: 9, itemId: 0xa0cdd2e9, count: 0x8001, level: 1 }, { slot: 31, itemId: I.iron, count: 3 }];
    const extra: E[] = [{ slot: 15, itemId: I.scraps, count: 0 }];
    const p = block({ main, extra, backpackPages: 2, between: Buffer.from([5, 0, 1, 0]), tail: Buffer.concat([Buffer.from([0x0f, 0x94, 0x37, 0x72, 0x99, 0, 0, 0, 2, 0, 3]), Buffer.alloc(11)]) });
    const r = read(p);
    expect(r.unknownItems).toBe(true);
    expect(r.character._invUnsure).toBe(false);
    expect(r.character.main.map((e: E) => e.slot)).toEqual([0, 1, 2, 9, 31]);
    expect(r.character.main[3]).toMatchObject({ itemId: 0xa0cdd2e9, count: 0x8001, level: 1 });
    expect(r.character.extra).toEqual([{ slot: 15, itemId: I.scraps, count: 0, flags: 0 }]);
    expect(Buffer.compare(buildCharacter(r.character), p)).toBe(0);
  });

  test("a full main inventory and backpack (40 + 32 stacks) read whole", () => {
    const main = Array.from({ length: 40 }, (_, slot) => ({ slot, itemId: [I.diamond, I.scraps, I.iron][slot % 3], count: 5 }));
    const extra = Array.from({ length: 32 }, (_, slot) => ({ slot, itemId: I.mana, count: 5 }));
    const p = block({ main, extra, between: Buffer.concat([U32_FIVE, U32_FIVE]) });
    const c = strict(p);
    expect([c.main.length, c.extra.length, c._invUnsure]).toEqual([40, 32, false]);
    expect(Buffer.compare(buildCharacter(c), p)).toBe(0);
  });
});
