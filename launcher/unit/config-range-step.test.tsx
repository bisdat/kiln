import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import Ajv2020 from "ajv/dist/2020";
import addFormats from "ajv-formats";
import type { SchemaSnapshot, SnapshotEntry } from "../src/api/shared";
import { SchemaForm, checkValue } from "../src/ui/SchemaForm";
import common from "../../schema/common.schema.json";
import configSchema from "../../schema/config-schema.schema.json";

const entry = (extra = {}): SnapshotEntry => ({ path: "fraction", type: "float", comment: null, default: 0.123, requiresRestart: false, synced: false, min: -0.35, max: 1.05, ...extra });
const schema = (e: SnapshotEntry) => ({ format: 1, source: "kiln", file: "step.json", id: "step", mod: "step", sections: [], entries: [e] }) as SchemaSnapshot;
const render = (e: SnapshotEntry) => renderToStaticMarkup(<SchemaForm schema={schema(e)} values={{ fraction: 0.127 }} onChange={() => { throw new Error("render must not commit"); }} />);

test("a config slider receives fractional step anchored at its negative minimum, without rounding its number box", () => {
  const html = render(entry({ step: 0.1 }));
  expect(html).toContain('min="-0.35" max="1.05" step="0.1"');
  expect(html).toContain('value="0.127"');
  expect(checkValue(entry({ step: 0.1 }), 0.127)).toBeNull();
  expect(checkValue(entry({ step: 0.1 }), 2)).not.toBeNull();
});

test("legacy and unsuitable step metadata retain the original slider increment", () => {
  const old = render(entry());
  for (const step of [undefined, null, 0, -0.1, NaN, Infinity, "bad", 2, 1e-300])
    expect(render(entry({ step }))).toBe(old);
  expect(render(entry({ type: "int", min: 3, max: 20, step: 0.5 }))).toBe(render(entry({ type: "int", min: 3, max: 20 })));
  expect(render(entry({ type: "int", min: 3.5, max: 20, step: 2 }))).toBe(render(entry({ type: "int", min: 3.5, max: 20 })));
});

test("the existing format-1 config schema permits only positive numeric optional step metadata", () => {
  const ajv = new Ajv2020({ strict: false });
  addFormats(ajv);
  ajv.addSchema(common);
  const validate = ajv.compile(configSchema);
  expect(validate(schema(entry()))).toBe(true);
  expect(validate(schema(entry({ step: 0.1 })))).toBe(true);
  for (const step of [0, -1, "0.1", null]) expect(validate(schema(entry({ step })))).toBe(false);
});
