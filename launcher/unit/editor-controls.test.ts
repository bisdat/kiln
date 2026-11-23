// Disabled is the caller contract for Inspector and the shared controls; Inventory/Equipment keep browsing enabled.
import { expect, test } from "bun:test";
import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
// @ts-ignore: the editor is plain JavaScript
import { Inspector } from "../src/editor/kiln/Inspector.jsx";
// @ts-ignore
import { EditorEnv } from "../src/editor/kiln/env.jsx";
// @ts-ignore
import { Stepper, Meter } from "../src/editor/kiln/parts.jsx";
import items from "../src/editor/data/items.json";
import { setLanguage } from "../src/i18n";

function allDisabled(html: string) {
  const controls = html.match(/<(?:button|input|select)\b[^>]*>/g) ?? [];
  expect(controls.length).toBeGreaterThan(0);
  for (const tag of controls) expect(tag).toMatch(/\bdisabled=""/);
}

test("Inspector disables every mutation control on desktop and Deck without hiding item content", () => {
  setLanguage("en");
  const id = Number(Object.keys(items).find((id) => (items as Record<string, { en: string }>)[id].en === "Eclipsed Blade"));
  const entry = { itemId: id, count: 2, level: 32, upgrades: 2, maxUpgrades: 4, durability: 120, maxDurability: 300,
    liquid: 640, maxLiquid: 1280, socket: "0000000000" };
  for (const deck of [false, true]) {
    const html = renderToStaticMarkup(createElement(EditorEnv.Provider, { value: { deck } },
      createElement(Inspector, { entry, disabled: true, onChange: () => {}, onRemove: () => {}, onDuplicate: () => {} })));
    expect(html).toContain("Eclipsed Blade");
    expect(html).toContain("120/300");
    expect(html).toContain("640/1280");
    allDisabled(html);
    if (!deck) expect(html.match(/role="slider"[^>]*aria-disabled="true"/g)).toHaveLength(2);
  }
});

test("standalone Stepper and Meter honor disabled in both environments", () => {
  for (const deck of [false, true]) {
    for (const [Control, props] of [[Stepper, { big: 10, min: 1 }], [Meter, {}]] as const) {
      const html = renderToStaticMarkup(createElement(EditorEnv.Provider, { value: { deck } },
        createElement(Control, { ...props, value: 120, max: 300, label: "Value", disabled: true, onChange: () => {} })));
      allDisabled(html);
    }
  }
});
