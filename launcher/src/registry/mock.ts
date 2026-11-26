// A normal browser session has no native registry report or plans. Playwright supplies synthetic fixtures.
import type { RegistryRollback } from "./api";

export async function readMockRegistryReport(): Promise<string | null> { return null; }
export async function mockRegistryRollback(opts: { clear: boolean; dryRun: boolean }): Promise<RegistryRollback> {
  return { plan: null, previous: null, rollback: null, pending: null, target: null, changes: [], changesUnknown: null,
    blocked: "No registry plan yet: start the game once with Kiln.", dryRun: opts.dryRun, wrote: false, cleared: opts.clear ? false : null };
}
