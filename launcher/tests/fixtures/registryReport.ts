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
