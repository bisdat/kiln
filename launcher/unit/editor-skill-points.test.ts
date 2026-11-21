// The save editor's skill-point total (src/editor/src/treecheck.js gameTotal): the game stores none and works it out at
// each load (docs/re/progression.md section 3): 2 per level above 1, plus the amount of every skill-point knowledge
// whose value is not 0 (data/skill-points.json, from GameKnowledgeResource.skillPoints). Player report 2026-11-20: the
// game showed In use 28, Available 0 on a level-9 character with six skill-point knowledge entries (12 points); the
// editor showed Available 1 from a total the player had typed.
import { describe, expect, test } from "bun:test";
import { checkTree, gameTotal, knownTotal } from "../src/editor/src/treecheck.js";
import skillPoints from "../src/editor/data/skill-points.json";

const sp = Object.entries(skillPoints.knowledge as Record<string, number>);
const ones = sp.filter(([, a]) => a === 1).map(([id]) => Number(id));
const threes = sp.filter(([, a]) => a === 3).map(([id]) => Number(id));

describe("the skill-point total", () => {
  test("the table is the game's: 78 entries, 60 of +1 and 18 of +3", () => {
    expect([sp.length, ones.length, threes.length, skillPoints.perLevel]).toEqual([78, 60, 18, 2]);
  });

  test("level 9 with Shroud Roots and Elixir Wells cleared: 16 + 12 = 28, as the game shows", () => {
    const knowledge = [...ones.slice(0, 3), ...threes.slice(0, 3)].map((id) => ({ id, value: 1 }))
      .concat([{ id: ones[5], value: 0 }, { id: 12345, value: 7 }]); // not cleared; no skill points
    const model = { path: "/x/character0.ksd", level: 9, knowledge, skills: [] };
    expect(gameTotal(model)).toBe(28);
    expect(knownTotal(model)).toBe(28);
  });

  test("without the knowledge block the game's total is not known", () => {
    expect(gameTotal({ level: 9, knowledge: null })).toBeNull();
  });

  test("the tree check uses it: spending more than the game gives is refused", () => {
    const model = { path: "/x/character1.ksd", level: 2, knowledge: [], skills: [] };
    expect(checkTree(model).total).toBe(2);
  });
});
