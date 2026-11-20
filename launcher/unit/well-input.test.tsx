// WellInput's filter use (ui/Form.tsx onClear): the clear (X) button only while the box has text, its name, and the rule
// that says whose Esc it is (escClearsFilter, also asked by ui/Modal.tsx). Clicks, focus and Esc in a real page:
// tests/filter-clear.spec.ts.   bun test unit/well-input.test.tsx
import { expect, test } from "bun:test";
import { renderToString } from "react-dom/server";
import { WellInput, escClearsFilter } from "../src/ui/Form";

const noop = () => {};
const html = (props: Partial<Parameters<typeof WellInput>[0]>) =>
  renderToString(<WellInput label="Filter" id="f" onChange={noop} {...props} />);
const clearButton = /<button[^>]*class="k-well-clear"[^>]*>/;

test("the clear button shows only while a filter box has text", () => {
  expect(html({ value: "", onClear: noop })).not.toMatch(clearButton);
  const filled = html({ value: "chest", onClear: noop });
  expect(filled).toMatch(clearButton);
  const button = filled.match(clearButton)![0];
  expect(button).toContain('type="button"');
  expect(button).toContain('aria-label="Clear filter"');
  expect(button).toContain('aria-controls="f"');
  expect(button).not.toContain("tabindex"); // in the tab order and a Deck focus candidate (deck/focus.ts)
  // a well without onClear stays as it was: no button, no mark for Esc listeners
  const plain = html({ value: "chest" });
  expect(plain).not.toMatch(clearButton);
  expect(plain).not.toContain("data-esc-clears");
  expect(filled).toContain('data-esc-clears=""');
});

test("the button's name follows the label, or clearLabel", () => {
  expect(html({ label: "Search items", value: "x", onClear: noop })).toContain('aria-label="Clear search items"');
  expect(html({ value: "x", onClear: noop, clearLabel: "Clear the console filter" })).toContain('aria-label="Clear the console filter"');
});

type Key = Parameters<typeof escClearsFilter>[0];
const box = (value: string, marked = true) => ({ value, getAttribute: (n: string) => (marked && n === "data-esc-clears" ? "" : null) });
const esc = (target: unknown, more: Partial<Key> = {}): Key =>
  ({ key: "Escape", ctrlKey: false, altKey: false, metaKey: false, shiftKey: false, isComposing: false, target, ...more }) as Key;

test("Esc in a filter box with text is the box's; an empty box leaves Esc to the page", () => {
  expect(escClearsFilter(esc(box("chest")))).toBe(true);
  expect(escClearsFilter(esc(box(" ")))).toBe(true);
  // empty: a dialog closes, the list or the page gets it, as before
  expect(escClearsFilter(esc(box("")))).toBe(false);
  // not a filter box (a plain field, the page, nothing)
  expect(escClearsFilter(esc(box("chest", false)))).toBe(false);
  expect(escClearsFilter(esc(null))).toBe(false);
  expect(escClearsFilter(esc({}))).toBe(false);
  // another key, a modifier, an IME composition
  expect(escClearsFilter(esc(box("chest"), { key: "Enter" }))).toBe(false);
  for (const m of ["ctrlKey", "altKey", "metaKey", "shiftKey", "isComposing"] as const) expect(escClearsFilter(esc(box("chest"), { [m]: true }))).toBe(false);
});
