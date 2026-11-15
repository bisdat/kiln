import type { StoppedStart } from "../api/commands";
export const stopKey = (s: StoppedStart) => `${s.pid}:${s.utc}:${s.exitCode}`;
export const stopSentence = (s: StoppedStart) => s.kind === "roster"
  ? `These mods add more NPCs to the Survivors roster than the game can hold${s.names.length ? `: ${s.names.join(", ")}` : "."}`
  : `${s.reason}${s.names.length ? ` (${s.names.join(", ")})` : ""}`;
export const stopFilter = (ids: string[]) => `ids:${ids.map(encodeURIComponent).join(",")}`;
export function stopIds(text: string): string[] | null {
  if (!text.startsWith("ids:")) return null;
  try { return text.slice(4).split(",").filter(Boolean).map(decodeURIComponent); } catch { return []; }
}
