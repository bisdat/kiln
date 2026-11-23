// Synthetic backpack headers must describe the worn bag, including when its container is empty.
import { expect, test } from 'bun:test';
import { editorMockHandlers } from '../src/api/mockEditor';
// @ts-ignore: the editor is plain JavaScript
import * as sv2c from '../src/editor/shared/sv2c-web.mjs';
// @ts-ignore
import { parseCharacter, buildCharacter } from '../src/editor/shared/character.mjs';
// @ts-ignore
import { replaceSlots } from '../src/editor/shared/inventory.mjs';
import items from '../src/editor/data/items.json';
import skills from '../src/editor/data/skills.json';

for (const [file, pages] of [['character0.ksd', 4], ['character1.ksd', 1]] as const) {
  test(`${file}: occupied and empty backpacks retain their complete page count`, async () => {
    const h = editorMockHandlers((code, message) => { throw Object.assign(Error(message), { code }); }, () => false);
    const saves = await h.editor_list_saves({}) as { path: string; file: string }[];
    const path = saves.find((s) => s.file === file)!.path;
    const before = new Uint8Array(await h.editor_read_file({ path }) as ArrayBuffer);
    await sv2c.init();
    const save = await sv2c.parse(before, () => { throw Error('Unexpected compression'); }, ['player_character']);
    const block = save.blocks.find((b: { name: string }) => b.name === 'player_character');
    const parse = (p: Buffer) => parseCharacter(p, (id: number) => Object.hasOwn(items, id), (id: number) => Object.hasOwn(skills.nodes, id));
    const c = parse(block.payload);
    expect(c._raw.between.at(-2)).toBe(pages);
    // The last legal destination must survive an actual rebuild and strict parse with every other field intact.
    const next = replaceSlots(c, [['main:0', null], [`extra:${pages * 8 - 1}`, c.main.find((e: { slot: number }) => e.slot === 0)]]);
    const rebuilt = buildCharacter(next);
    const reread = parse(rebuilt);
    expect(reread.main).toEqual(next.main);
    expect(reread.extra).toEqual(next.extra);
    expect(reread.equipment).toEqual(c.equipment);
    expect(reread._invUnsure).toBe(false);
  });
}
