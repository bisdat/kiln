import type { GameplayStatus } from "./types";

export function zoneHookNotices(status?: GameplayStatus): string[] {
  if (!status?.available) return [];
  const notices: string[] = [];
  for (const [group, flag, label] of [["build", "buildHooks", "Building and breaking"], ["terraform", "terraformHook", "Digging"], ["spawn", "spawnHook", "Enemy spawns"]] as const) {
    const row = status.hookGroups?.[group];
    if (row?.state === "installed" || (!row || row.state === "unknown") && status[flag]) continue;
    const text = row?.state === "idle" ? "not in use yet" : row?.state === "waiting" ? `waiting: ${row.reason}` :
      row?.state === "refused" ? `refused: ${row.reason.replace(/^refused:\s*/i, "")}` : "installation status unknown on this server version";
    notices.push(`${label}: ${text}.`);
  }
  // CraftRecipe observes completed recipes. Craft protection, like opening and looting, uses Pre interception.
  if (status.intercept === false) notices.push("Opening, looting and crafting: Pre interception unavailable.");
  return notices;
}
