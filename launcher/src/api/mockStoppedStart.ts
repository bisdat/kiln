// Mock state-file boundary. Rust stopped_start::read is authoritative; these fixtures let browser tests script
// malformed and stale evidence without manufacturing a notice directly.
import type { StoppedStart } from "./commands";
export type StopScript = "roster" | "boot-mod" | "unknown" | "garbled" | "stale" | "quit";
export function mockStoppedFile(kind: StopScript, pid: number, at: number): string {
  if (kind === "garbled") return "{";
  if (kind === "quit") return "{}";
  return JSON.stringify({ format: kind === "unknown" ? 99 : 1, pid: kind === "stale" ? pid - 1 : pid,
    utc: new Date(kind === "stale" ? at - 60_000 : at).toISOString(),
    kind: kind === "boot-mod" ? "boot-mod" : "roster", mods: ["new-items", "big-stacks"],
    reason: kind === "boot-mod" ? "A required boot mod could not prepare its content." : "The roster holds at most 128 NPCs.",
    advice: "Switch one of these mods off before starting again.", exitCode: kind === "boot-mod" ? 19201 : 19202 });
}
export function readMockStop(text: string, pid: number, began: number, ended: number): StoppedStart | null {
  if (new TextEncoder().encode(text).length > 256 * 1024) return null;
  try {
    const v = JSON.parse(text);
    const time = Date.parse(v.utc);
    if (v.format !== 1 || v.pid !== pid || !Number.isFinite(time) || time < began || time > ended
      || !["roster", "boot-mod"].includes(v.kind) || typeof v.reason !== "string" || !v.reason.trim()
      || typeof v.advice !== "string" || !Array.isArray(v.mods) || !v.mods.every((m: unknown) => typeof m === "string")) return null;
    return { kind: v.kind, pid: v.pid, utc: v.utc, reason: v.reason, advice: v.advice, mods: v.mods,
      names: v.mods.map((id: string) => ({ "new-items": "New Items", "big-stacks": "Big Stacks" } as Record<string, string>)[id] ?? id), exitCode: v.exitCode };
  } catch { return null; }
}
