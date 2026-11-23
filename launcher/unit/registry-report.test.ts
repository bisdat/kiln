import { describe, expect, test } from "bun:test";
import { emptyRegistryFixture, nativeShapedReport, registryFixture } from "../tests/fixtures/registryReport";
import { parseRegistryReport, reportFreshness, REPORT_LIMITS } from "../src/registry/report";

const parse = (r: unknown) => parseRegistryReport(JSON.stringify(r));
describe("untrusted registry report", () => {
  test("reads the native format and ordered owners/operations", () => {
    const r = parse(registryFixture());
    expect(r.fields[0].baseOrigin).toBe("archive");
    expect(r.fields[0].writers.map(w => w.owner)).toEqual(["earlier", "stacker"]);
    expect(r.fields[0].writers[1].operation).toBe('{"mul":2}');
    expect(r.fields[0].winner).toBe("stacker");
    expect(r.fields[0].result).toBe("40");
    expect(r.baseOrigin).toBe("live");
  });
  test("empty is a valid report, not a missing file", () => expect(parse(emptyRegistryFixture()).empty).toBe(true));
  test("refusals retain RestartRequired and Unproven reasons", () => {
    expect(parse(registryFixture()).refused.map(x => x.reason)).toEqual(registryFixture().refused.map(x => x.reason));
  });
  test("does not upgrade unknown certainty to certain", () => {
    const r = registryFixture(); r.conflicts[0].certainty = "future";
    expect(parse(r).conflicts.map(x => x.certainty)).toEqual(["Unknown", "Best-effort drift"]);
  });
  test("journal naming is not certain observation", () => {
    const r = registryFixture(); r.fields[0].writers[0].certainty = "named";
    expect(parse(r).fields[0].writers[0].certainty).toBe("Named from journal");
  });
  test.each(["null", "[]", "{}", "{", '{"format":2}'])("rejects invalid or unsupported document %s", text => {
    expect(() => parseRegistryReport(text)).toThrow();
  });
  test("rejects oversized input before JSON parsing", () => expect(() => parseRegistryReport(" ".repeat(REPORT_LIMITS.bytes + 1))).toThrow("8 MiB"));
  test("an oversized report says where to read it in full", () => expect(() => parseRegistryReport(" ".repeat(REPORT_LIMITS.bytes + 1))).toThrow("kiln/state/registry.json"));
  test("a native-shaped 5,000-field report (about 1.4 MiB) parses", () => {
    const text = JSON.stringify(nativeShapedReport(5000));
    expect(text.length).toBeGreaterThan(1024 * 1024);
    const out = parseRegistryReport(text);
    expect(out.fields.length).toBe(REPORT_LIMITS.rows);
    expect(out.omitted).toBe(5000 - REPORT_LIMITS.rows);
  });
  test("a native-shaped report at the native field cap (20,000 rows) parses and counts what is left out", () => {
    const text = JSON.stringify(nativeShapedReport(20_000, 1, 26_000));
    expect(text.length).toBeLessThanOrEqual(REPORT_LIMITS.bytes);
    const out = parseRegistryReport(text);
    expect(out.fields.length).toBe(REPORT_LIMITS.rows);
    expect(out.omitted).toBe(26_000 - REPORT_LIMITS.rows);
  });
  test("hostile structure within the byte cap stays bounded", () => {
    expect(() => parseRegistryReport("[" + "[],".repeat(1_000_000) + "[]]")).toThrow("structure limit");
  });
  test("caps nesting before JSON parsing", () => expect(() => parseRegistryReport("[".repeat(70) + "0" + "]".repeat(70))).toThrow("nesting"));
  test("ignores unknown members without rendering them", () => {
    expect(JSON.stringify(parse({ ...registryFixture(), offset: "0xDEADBEEF", gameFunction: "SecretFunction" }))).not.toContain("SecretFunction");
  });
  test("refuses malformed rows instead of silently showing an empty report", () => {
    const r = { ...registryFixture(), fields: [null] };
    expect(() => parse(r)).toThrow();
  });
  test("bounds rows, writers and text and discloses omissions", () => {
    const r = registryFixture();
    r.fields = Array.from({ length: REPORT_LIMITS.rows + 1 }, () => ({ ...r.fields[0] }));
    r.fields[0].writers = Array.from({ length: 80 }, (_, n) => ({ ...r.fields[0].writers[0], mod: `writer${n}` }));
    r.refused[0].reason = "x".repeat(10000);
    const out = parse(r);
    expect(out.fields.length).toBe(REPORT_LIMITS.rows);
    expect(out.fields[0].writers.length).toBe(REPORT_LIMITS.writers);
    expect(out.fields[0].writers.at(-1)?.owner).toBe("writer79");
    expect(out.fields[0].writersOmitted).toBe(16);
    expect(out.refused[0].reason.length).toBeLessThanOrEqual(REPORT_LIMITS.text + 1);
    expect(out.omitted).toBeGreaterThan(0);
  });
  test("native truncation and list omission counts are visible", () => {
    const r = registryFixture();
    const field = { ...r.fields[0], writersLeftOut: 4, resultEntriesLeftOut: 6, base: { ...r.fields[0].base, entriesLeftOut: 3 } };
    const out = parse({ ...r, fields: [field], truncated: { ...r.truncated, fields: 20 } });
    expect(out.omitted).toBe(20);
    expect(out.fields[0].writersOmitted).toBe(4);
    expect(out.fields[0].result).toContain("6 omitted");
    expect(out.fields[0].base).toContain("3 omitted");
  });
  test("writer chains cut by native or by this view are counted for the report banner", () => {
    const r = registryFixture();
    const out = parse({ ...r, truncated: { ...r.truncated, writers: 7 } });
    expect(out.omitted).toBe(0);
    expect(out.writersOmitted).toBe(7);
    const many = { ...r.fields[0], writers: Array.from({ length: 70 }, (_, n) => ({ ...r.fields[0].writers[0], mod: `w${n}` })) };
    expect(parse({ ...r, fields: [many], truncated: { ...r.truncated, writers: 7 } }).writersOmitted).toBe(13);
  });
  test("raw paths and native offsets never enter the view model", () => {
    const r = registryFixture();
    const field = { ...r.fields[0], raw: true, field: "SecretSetup.Offset" };
    const out = parse({ ...r, fields: [field], refused: [{ ...r.refused[0], reason: "Unmapped at RVA 0xDEADBEEF" }] });
    expect(JSON.stringify(out)).not.toContain("SecretSetup");
    expect(JSON.stringify(out)).not.toContain("DEADBEEF");
    expect(out.fields[0].field).toBe("Advanced field (details hidden)");
  });
  test("raw conflict/refusal paths stay hidden even without a retained field row", () => {
    const r = registryFixture();
    const out = parse({ ...r, fields: [],
      conflicts: [{ ...r.conflicts[0], field: "items[game:fixture_stack].SecretSetup.Offset", raw: true, text: "SecretSetup.Offset changed" }],
      refused: [{ ...r.refused[0], registry: "items", field: "SecretSetup.Offset", raw: true, reason: "Unproven: SecretSetup.Offset (mapping missing)" }],
    });
    expect(JSON.stringify(out)).not.toContain("SecretSetup");
  });
  test("a refusal of a misspelled Kiln field keeps its native reason and is not called Advanced", () => {
    const r = registryFixture();
    const out = parse({ ...r, refused: [{ mod: "typo", source: "content/a.json", stage: "alter", n: 0, registry: "items", id: "game:fixture_stack",
      field: "maxStak", reason: "items has no field \"maxStak\"" }] });
    expect(out.refused[0].reason).toBe("items has no field \"maxStak\"");
    expect(out.refused[0].field).toContain("maxStak");
    expect(out.refused[0].field).toContain("Field unknown to this launcher");
    expect(JSON.stringify(out.refused)).not.toContain("Advanced");
  });
  test("a field or conflict a newer Kiln names is labelled unknown to this launcher, not Advanced", () => {
    const r = registryFixture();
    const out = parse({ ...r, fields: [{ ...r.fields[0], field: "futureField" }],
      conflicts: [{ ...r.conflicts[0], field: "items[game:fixture_stack].futureField", text: "stacker composes on earlier" }] });
    expect(out.fields[0].field).toBe("futureField (Field unknown to this launcher)");
    expect(out.conflicts[0].field).toBe("items[game:fixture_stack].futureField (Field unknown to this launcher)");
    expect(out.conflicts[0].text).toBe("stacker composes on earlier");
  });
  test("a refusal the runtime marks raw stays hidden", () => {
    const r = registryFixture();
    const out = parse({ ...r, refused: [{ mod: "adv", source: "", stage: "alter", n: 0, registry: "items", field: "SecretSetup.Member", raw: true,
      reason: "game:x: no entry" }] });
    expect(JSON.stringify(out.refused)).not.toContain("SecretSetup");
    expect(out.refused[0].field).toBe("Advanced field (details hidden)");
  });
  test("a Kiln field's conflict text never names a raw path a mod wrote", () => {
    const r = registryFixture();
    const out = parse({ ...r, conflicts: [
      { ...r.conflicts[0], kind: "overridden", text: "items[game:fixture_stack].maxStack: b sets 5 over a's raw path SecretDesc.m_stackSize (one place in the game on two scales; the later value wins)" },
      { ...r.conflicts[0], kind: "exclusive", text: "Conflict (one place, two scales): items[game:fixture_stack].maxStack and raw path SecretDesc.m_stackSize are one place in the game, which Kiln converts between them; a writes raw path SecretDesc.m_stackSize there and no conversion of a relative change is known here: b's relative edit is refused (a value would win)" },
    ] });
    expect(JSON.stringify(out.conflicts)).not.toContain("SecretDesc");
    expect(out.conflicts[0].text).toContain("raw path (details hidden)");
    expect(out.conflicts[1].text).toContain("items[game:fixture_stack].maxStack and raw path (details hidden) are one place");
  });
  test("a Kiln field's refusal reason never names a raw path a mod wrote", () => {
    const r = registryFixture();
    const out = parse({ ...r, refused: [
      { ...r.refused[1], reason: "Conflict (one place, two scales): items[game:fixture_stack].maxStack and raw path SecretDesc.m_stackSize are one place" },
      { ...r.refused[1], field: "", reason: "game:x: raw path \"SecretDesc.Member\" does not resolve on this game build (no member)" },
      { ...r.refused[1], field: "", reason: "items[game:x].raw:secretdesc.member: needs a restart; this edit was not applied" },
      { ...r.refused[1], field: "", reason: "Conflict (exclusive): a also writes items[game:x].SecretDesc.Member (raw); the later edit is refused" },
    ] });
    expect(JSON.stringify(out.refused).toLowerCase()).not.toContain("secretdesc");
    expect(out.refused[0].reason).toContain("items[game:fixture_stack].maxStack");
  });
  test("HTML is kept as plain text, never interpreted", () => {
    const r = registryFixture(); r.refused[0].reason = '<img src=x onerror="alert(1)">';
    expect(parse(r).refused[0].reason).toBe(r.refused[0].reason);
  });
  test("deep or wide values are bounded, not recursively stringified wholesale", () => {
    const r = registryFixture();
    const out = parse({ ...r, fields: [{ ...r.fields[0], result: Array(500).fill("value") }] });
    expect(out.fields[0].result.length).toBeLessThan(2000);
    expect(out.fields[0].result).toContain("omitted");
  });
});
describe("snapshot freshness", () => {
  const now = Date.now();
  const written = new Date(now - 1000).toISOString();
  test("stopped game is stale", () => expect(reportFreshness(written, { running: false }, now)).toContain("not running"));
  test("report from a previous process start is stale", () => expect(reportFreshness(written, { running: true, started: new Date(now).toISOString() }, now)).toContain("current game start"));
  test("old snapshot is stale even when the game is running", () => expect(reportFreshness(new Date(now - 86400001).toISOString(), { running: true }, now)).toContain("24 hours"));
  test("future clock is not presented as fresh", () => expect(reportFreshness(new Date(now + 600000).toISOString(), { running: true }, now)).not.toBeNull());
  test("unknown process state is explicit", () => expect(reportFreshness(written, undefined, now)).toContain("cannot be verified"));
  test("invalid process-start timestamp cannot establish freshness", () => expect(reportFreshness(written, { running: true, started: "invalid" }, now)).toContain("cannot be verified"));
  test("snapshot from this start is not stale", () => expect(reportFreshness(written, { running: true, started: new Date(now - 2000).toISOString() }, now)).toBeNull());
});
