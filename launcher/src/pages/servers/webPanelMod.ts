// "Open web panel" (0.2.0-dev.31.14): the web panel mod is Kilnwarden (`kilnwarden`), formerly Kiln Web Admin
// (`kiln-webadmin`, the 0.1.0 test builds; launchers 31.2 to 31.13 know only that id). Either one, enabled, shows the
// button; with both enabled it is Kilnwarden's, as kiln_manage::admin::web_panel::ids looks for it first (Kilnwarden
// then stays off by itself and says so in kiln.log, and the backend opens the old copy's panel). Pure, for the unit tests.
import type { ServerMod } from "../../api/commands";

/** Kilnwarden first. */
export const WEB_PANEL_MOD_IDS = ["kilnwarden", "kiln-webadmin"] as const;
export type WebPanelModId = (typeof WEB_PANEL_MOD_IDS)[number];

/** The enabled web panel mod on the server, if any. */
export function webPanelMod(mods: Pick<ServerMod, "id" | "enabled">[] | undefined): WebPanelModId | null {
  return WEB_PANEL_MOD_IDS.find((id) => mods?.some((m) => m.id === id && m.enabled)) ?? null;
}
