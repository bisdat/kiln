import { expect, test } from "bun:test";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { hasUnprovenRecovery, RestoreDataOffer } from "../src/components/RestoreDataOffer";
function render(plan: unknown, latest: unknown = null) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  if (plan) client.setQueryData(["eml_data_plan", "test"], plan);
  client.setQueryData(["eml_data_latest", "test"], latest);
  return renderToStaticMarkup(<QueryClientProvider client={client}><RestoreDataOffer gameId="test" /></QueryClientProvider>);
}
test("restore action needs a file plan", () => {
  expect(render(null)).toBe("");
  expect(render({ fingerprint: "reviewed", files: [] })).toContain("Restore the game&#x27;s data");
});
test("a saved move offers undo and names each file after reopening", () => {
  const html = render(null, { folder: "kiln/eml/displaced/restore-123", files: [{ from: "enshrouded_055.dat", size: 7, sha256: "x" }] });
  expect(html).toContain("Undo move");
  expect(html).toContain("enshrouded_055.dat");
  expect(html).toContain("fresh backup");
});

test("only explicit writer evidence offers recovery guidance", () => {
  expect(hasUnprovenRecovery({ reason: "unproven extra files" })).toBe(false);
  expect(hasUnprovenRecovery({ recovery: { unproven: false } })).toBe(false);
  expect(hasUnprovenRecovery({ recovery: { unproven: true } })).toBe(true);
});
