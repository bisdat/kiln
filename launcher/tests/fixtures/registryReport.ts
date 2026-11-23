// Synthetic Kiln identifiers only; no extracted game data.
export function registryFixture() {
  return {
    format: 1, pid: 4242, written: new Date().toISOString(), generation: 2, phase: "worldLoad",
    schema: { registrySchema: 1, registries: { items: "0.1.0" } },
    regmap: { side: "client", exeSha256: "f".repeat(64), record: null, verified: true },
    base: { origin: "live", writer: "none" }, order: [], orderSource: "kiln.host", plan: null,
    fields: [{ registry: "items", id: "game:fixture_stack", field: "maxStack", applies: "worldLoad",
      base: { origin: "archive", value: 10 }, writers: [
        { kind: "oldPack", mod: "earlier", op: 20, value: 20, certainty: "certain" },
        { kind: "registry", mod: "stacker", stage: "alter", n: 0, op: { mul: 2 }, value: 40, certainty: "certain" },
      ], result: 40, winner: "stacker" }],
    conflicts: [
      { registry: "items", id: "game:fixture_stack", field: "items[game:fixture_stack].maxStack", kind: "composedOn", winner: "stacker", loser: "earlier", certainty: "certain", text: "stacker composes on earlier" },
      { registry: "items", id: "game:fixture_stack", field: "items[game:fixture_stack].maxStack", kind: "skipped", winner: null, loser: "stacker", certainty: "bestEffort", text: "Another writer changed the field; preserved for this world" },
    ],
    refused: [
      { mod: "late", source: "", stage: "alter", n: 0, registry: "balance", field: "levelCap", reason: "RestartRequired: balance.levelCap is read at game start" },
      { mod: "unknown", source: "", stage: "alter", n: 1, registry: "items", field: "maxStack", reason: "Unproven: items.maxStack (no verified mapping for this build)" },
    ],
    added: [], removed: [], counts: { fields: 1, conflicts: 2, refused: 2, added: 0, removed: 0 },
    truncated: { fields: 0, conflicts: 0, refused: 0, added: 0, removed: 0, writers: 0 },
  };
}
export function emptyRegistryFixture() {
  const r = registryFixture();
  return { ...r, fields: [], conflicts: [], refused: [], counts: { fields: 0, conflicts: 0, refused: 0, added: 0, removed: 0 } };
}
/** A report in the shape native/services/content/registry/report.cpp writes for a large rebalance mod set: `count` field
 * rows (all retained up to the native cap of 20,000), each with `writers` registry writers. Synthetic names only. */
export function nativeShapedReport(count: number, writers = 1, total = count) {
  const r = registryFixture();
  const fields = Array.from({ length: count }, (_, n) => {
    const id = `game:fixture_item_${String(n).padStart(5, "0")}`;
    return {
      registry: "items", id, field: n % 2 ? "maxStack" : "weight", applies: "worldLoad",
      base: { origin: "archive", value: 10 + (n % 50) },
      writers: Array.from({ length: writers }, (_, w) => ({
        kind: "registry", mod: `rebalance_${w}`, stage: "alter", source: "content/rebalance.json", n: n * writers + w,
        op: { mul: 1.5 }, value: 15 + (n % 50) + w, certainty: "certain",
      })),
      result: 15 + (n % 50) + writers - 1, winner: `rebalance_${writers - 1}`,
    };
  });
  return {
    ...r, kiln: "0.2.0-dev.31",
    regmap: { side: "client", exeSha256: "f".repeat(64), record: `regmap/client/${"f".repeat(64)}.json`, verified: true },
    order: Array.from({ length: writers }, (_, w) => ({ mod: `rebalance_${w}`, version: "1.0.0", editsHash: "9c1f" })),
    fields, conflicts: [], refused: [],
    counts: { fields: total, conflicts: 0, refused: 0, added: 0, removed: 0 },
    truncated: { fields: total - count, conflicts: 0, refused: 0, added: 0, removed: 0, writers: 0 },
  };
}
