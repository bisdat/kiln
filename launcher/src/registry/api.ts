// Read-only command. Fixed game-relative path and byte cap live on the native side too.
import { invoke } from "@tauri-apps/api/core";
import { isTauri, toKilnError, type GameId } from "../api/commands";

export async function readRegistryReport(gameId: GameId): Promise<string | null> {
  try {
    return isTauri() ? await invoke<string | null>("registry_report_read", { gameId })
      : await (await import("./mock")).readMockRegistryReport();
  } catch (e) { throw toKilnError(e); }
}
