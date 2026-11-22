import { beforeEach, expect, test } from "bun:test";
import { Children, createElement, isValidElement, type ReactElement, type ReactNode } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { editorMockHandlers } from "../src/api/mockEditor";
// @ts-ignore: the editor is plain JavaScript
import { Inspector } from "../src/editor/kiln/Inspector.jsx";
// @ts-ignore
import { EditorEnv } from "../src/editor/kiln/env.jsx";
// @ts-ignore
import { Stepper, UpgradePick, RarityPick } from "../src/editor/kiln/parts.jsx";
// @ts-ignore
import { newEntry } from "../src/editor/src/gamedata.js";
// @ts-ignore
import api from "../src/editor/src/tauri-api.js";
import { setLanguage } from "../src/i18n";
// @ts-ignore
import { parseCharacter, buildCharacter } from "../src/editor/shared/character.mjs";
// @ts-ignore
import * as sv2c from "../src/editor/shared/sv2c-web.mjs";
import items from "../src/editor/data/items.json";
import skills from "../src/editor/data/skills.json";

type Entry = { slot: number; itemId: number; count: number; level?: number; upgrades?: number; maxUpgrades?: number; socket?: string };
type Element = ReactElement<Record<string, any>>;
beforeEach(() => setLanguage("en"));
const isItem = (id: number) => Object.hasOwn(items, id);
const isSkill = (id: number) => Object.hasOwn(skills.nodes, id);
const reject = (code: string, message: string): never => { throw Object.assign(new Error(message), { code }); };

async function character() {
  const h = editorMockHandlers(reject as never, () => false);
  const list = await h.editor_list_saves({}) as { path: string }[];
  const bytes = new Uint8Array(await h.editor_read_file({ path: list[0].path }) as ArrayBuffer);
  await sv2c.init();
  const save = await sv2c.parse(bytes, () => { throw new Error("compressed"); }, ["player_character"]);
  return parseCharacter(save.blocks.find((b: { name: string }) => b.name === "player_character").payload, isItem, isSkill);
}

function elements(node: ReactNode): Element[] {
  return Children.toArray(node).flatMap((child) => isValidElement(child)
    ? [child as Element, ...elements((child as Element).props.children)] : []);
}

// Read the real Inspector's controls without a browser or module mocks. The probe supplies React's render context
// for ItemForm's useEditorEnv; its returned element tree retains the actual edit callbacks.
function form(entry: Entry, onChange: (entry: Entry) => void, env: Record<string, any> = { deck: false }): Element[] {
  const inspector = Inspector({ entry, onChange });
  const item = elements(inspector).find((e) => typeof e.type === "function")!;
  let controls: Element[] = [];
  function Probe() {
    controls = elements((item.type as (props: unknown) => ReactNode)(item.props));
    return null;
  }
  renderToStaticMarkup(createElement(EditorEnv.Provider, { value: env }, createElement(Probe)));
  return controls;
}

test("an upgrade from a loaded level-255 item cannot wrap its saved level byte to zero", async () => {
  const c = await character();
  const original = { ...c.main.find((e: Entry) => e.slot === 24), level: 255, upgrades: 0, maxUpgrades: 5 };
  let edited: Entry = original;
  const controls = form(original, (e) => { edited = e; });
  controls.find((e) => e.type === UpgradePick)!.props.onChange(1);
  c.main = c.main.map((e: Entry) => e.slot === original.slot ? edited : e);
  const roundTrip = parseCharacter(buildCharacter(c), isItem, isSkill);
  expect(roundTrip.main.find((e: Entry) => e.slot === original.slot).level).toBe(edited.level);
  expect(edited.level).toBeLessThanOrEqual((items as Record<string, { maxLevel: number }>)[edited.itemId].maxLevel + 1);
});

const hatId = Number(Object.entries(items).find(([, it]) => it.maxLevel === 10 && it.slot === "Armour_Head")![0]);
const gemId = Number(Object.entries(items).find(([, it]) => it.category === "WeaponGem")![0]);
const hat = (changes: Partial<Entry> = {}): Entry => ({ slot: 0, itemId: hatId, count: 1, level: 12, upgrades: 2, maxUpgrades: 5, ...changes });
const control = (controls: Element[], type: unknown, label?: string) => controls.find((e) => e.type === type && (!label || e.props.label === label))!;

function renderControl(element: Element, env: Record<string, any> = { deck: false }): Element[] {
  let controls: Element[] = [];
  function Probe() {
    controls = elements((element.type as (props: unknown) => ReactNode)(element.props));
    return null;
  }
  renderToStaticMarkup(createElement(EditorEnv.Provider, { value: env }, createElement(Probe)));
  return controls;
}

test("upgrades and rarity preserve the base level, within the item's range and rarity", () => {
  let edited = hat();
  let controls = form(edited, (e) => { edited = e; });
  control(controls, UpgradePick).props.onChange(5);
  expect(edited).toMatchObject({ level: 15, upgrades: 5, maxUpgrades: 5 });
  controls = form(edited, (e) => { edited = e; });
  control(controls, RarityPick).props.onChange(1);
  expect(edited).toMatchObject({ level: 11, upgrades: 1, maxUpgrades: 1 });
});

test("the level input caps the base level plus applied upgrades, not the base level alone", () => {
  let edited = hat();
  const level = control(form(edited, (e) => { edited = e; }), Stepper, "Level");
  expect(level.props.max).toBe(12);
  const input = renderControl(level).find((e) => e.type === "input")!;
  input.props.onChange({ target: { value: "256" } }); // typed or pasted
  expect(edited.level).toBe(12);
  input.props.onChange({ target: { value: "11.9" } });
  expect(edited.level).toBe(11);
});

test("a keyboard step and the Deck number pad use the same per-item limit", () => {
  let edited = hat({ level: 11 });
  let level = control(form(edited, (e) => { edited = e; }), Stepper, "Level");
  const plus = renderControl(level).find((e) => e.props["aria-label"] === "+1")!;
  plus.props.onClick(); // the same handler is activated by Enter/Space or Deck A
  expect(edited.level).toBe(12);
  let pad: Record<string, any> = {};
  const env = { deck: true, askNumber: (o: Record<string, any>) => { pad = o; } };
  level = control(form(edited, (e) => { edited = e; }, env), Stepper, "Level");
  renderControl(level, env).find((e) => e.props["data-deck-id"])!.props.onClick();
  expect(pad.max).toBe(12);
  pad.onDone(999);
  expect(edited.level).toBe(12);
  pad.onDone(11.9);
  expect(edited.level).toBe(11);
});

test("socketed gems and catalog gems use the gem's own maximum", () => {
  const gemMax = (items as Record<string, { maxLevel: number }>)[gemId].maxLevel;
  const bytes = Buffer.alloc(5);
  bytes.writeUInt32LE(gemId);
  bytes[4] = 200;
  let edited = hat({ socket: bytes.toString("hex") });
  const level = control(form(edited, (e) => { edited = e; }), Stepper, "Gem level");
  expect(level.props.value).toBe(200); // loaded value is shown, not normalized on render
  expect(level.props.max).toBe(gemMax);
  renderControl(level).find((e) => e.type === "input")!.props.onChange({ target: { value: "999" } });
  expect(Buffer.from(edited.socket!, "hex")[4]).toBe(gemMax);
  expect(newEntry(gemId, 0, 1, undefined, 999).level).toBe(gemMax);
});

test("loaded out-of-range levels, upgrades and rarity are not changed by rendering or an unrelated edit", async () => {
  const c = await character();
  const original = hat({ level: 255, upgrades: 7, maxUpgrades: 9, count: 2 });
  c.main = [{ ...original }];
  const before = buildCharacter(c);
  let edited = original;
  let calls = 0;
  const controls = form(original, (e) => { calls++; edited = e; });
  expect(calls).toBe(0);
  expect(control(controls, Stepper, "Level").props.value).toBe(255);
  const upgrades = control(controls, UpgradePick);
  expect(upgrades.props.value).toBe(7);
  expect(upgrades.props.max).toBe(5);
  expect(renderToStaticMarkup(control(controls, RarityPick))).toContain(">9</span>");
  expect(elements(controls).some((e) => e.props.className === "ke-note warn")).toBe(false);
  control(controls, Stepper, "Amount").props.onChange(1);
  expect(edited).toMatchObject({ level: 255, upgrades: 7, maxUpgrades: 9, count: 1 });
  c.main = [original];
  expect(Buffer.compare(buildCharacter(parseCharacter(before, isItem, isSkill)), before)).toBe(0);
});

test("editing excessive upgrades caps them to rarity instead of offering the saved excessive count", () => {
  let edited = hat({ level: 255, upgrades: 7, maxUpgrades: 9 });
  control(form(edited, (e) => { edited = e; }), UpgradePick).props.onChange(255);
  expect(edited).toMatchObject({ level: 15, upgrades: 5, maxUpgrades: 9 }); // rarity was not edited
});

test("a catalog template is a new item and bounds its level, applied upgrades and rarity", () => {
  const entry = newEntry(hatId, 3, 1, hat({ level: 255, upgrades: 255, maxUpgrades: 255 }));
  expect(entry).toMatchObject({ slot: 3, count: 1, level: 5, upgrades: 5, maxUpgrades: 5 });
});

test("unknown, incomplete and modded item data fall back to the byte bound", () => {
  const table = items as Record<string, any>;
  const original = table[hatId];
  for (const info of [undefined, { ...original, maxLevel: undefined }, { ...original, rarity: undefined }, { ...original, modded: true }]) {
    try {
      if (info) table[hatId] = info; else delete table[hatId];
      let edited = hat({ level: 255, upgrades: 0, maxUpgrades: 255 });
      const controls = form(edited, (e) => { edited = e; });
      expect(control(controls, Stepper, "Level").props.max).toBe(255);
      expect(control(controls, UpgradePick).props.max).toBe(255);
      control(controls, UpgradePick).props.onChange(256);
      expect(edited.upgrades).toBe(255);
      expect(edited.level).toBe(255);
    } finally { table[hatId] = original; }
  }
});

test("even item data above the byte range cannot make catalog levels overflow", () => {
  const table = items as Record<string, any>;
  const original = table[hatId];
  try {
    table[hatId] = { ...original, minLevel: 300, maxLevel: 999 };
    expect(newEntry(hatId, 0, 1, undefined, 999).level).toBe(255);
  } finally { table[hatId] = original; }
});

test("the real save API keeps byte-sized values above the catalog limits", async () => {
  const [file] = await api.listSaves();
  const model = await api.openSave(file.path);
  const index = model.main.findIndex((e: Entry) => e.slot === 24);
  const changed = { ...model, main: model.main.map((e: Entry, i: number) => i === index ? { ...e, level: 255, upgrades: 7, maxUpgrades: 9 } : e) };
  const saved = await api.writeSave(file.path, changed);
  expect(saved.model.main[index]).toMatchObject({ level: 255, upgrades: 7, maxUpgrades: 9 });
});

test("the real save API refuses non-byte levels and upgrades before serialization", async () => {
  const [file] = await api.listSaves();
  const model = await api.openSave(file.path);
  for (const [key, value] of [["level", 256], ["level", -1], ["level", 1.5], ["level", NaN], ["level", Infinity], ["upgrades", 256], ["maxUpgrades", 256]] as const) {
    const changed = { ...model, main: model.main.map((e: Entry, i: number) => i === 0 ? { ...e, [key]: value } : e) };
    await expect(api.writeSave(file.path, changed)).rejects.toThrow("Item levels and upgrades must be whole numbers from 0 to 255.");
  }
});
