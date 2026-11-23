// A server's sections (the Servers redesign, mockup A, 2026-11-22): seven tabs under the server's head replace the
// grouped side list (21 entries): Overview, Console, Players, World, Mods, Backups, Settings. A section with more than
// one page shows its pages as a second row of tabs. Every entry is a route under /servers/:id/; the routes did not
// change, so old links keep working. Configs, Mods and the console are merged pages (live while the server runs, the
// files while it is stopped).

import { t } from "../i18n";

export interface ServerNavItem {
  /** The word in the current language (a getter: read it at render). */
  readonly label: string;
  /** Route under /servers/:id/. */
  to: string;
}

export interface ServerSection {
  id: "overview" | "console" | "players" | "world" | "mods" | "backups" | "settings";
  readonly label: string;
  /** The section's pages; the first is where its tab goes. */
  tabs: ServerNavItem[];
}

const item = (key: string, to: string): ServerNavItem => ({ get label() { return t(key); }, to });

export const SERVER_SECTIONS: ServerSection[] = [
  { id: "overview", get label() { return t("shell.serverNav.overview"); }, tabs: [item("shell.serverNav.overview", "overview")] },
  { id: "console", get label() { return t("shell.serverNav.console"); }, tabs: [
    item("shell.serverNav.console", "console"),
    item("shell.serverNav.logs", "logs"),
    item("shell.serverNav.tickStats", "ticks"),
    item("shell.serverNav.crashes", "admin/crashes"),
  ] },
  { id: "players", get label() { return t("shell.serverNav.players"); }, tabs: [
    item("shell.serverNav.players", "admin/players"),
    item("shell.serverNav.permissions", "admin/permissions"),
    item("shell.serverNav.combat", "admin/combat"),
  ] },
  { id: "world", get label() { return t("shell.serverNav.world"); }, tabs: [
    item("shell.serverNav.map", "admin/map"),
    item("shell.serverNav.zones", "admin/zones"),
    item("shell.serverNav.spawn", "admin/spawn"),
    item("shell.serverNav.creatures", "admin/creatures"),
    item("shell.serverNav.worldRules", "admin/world"),
  ] },
  { id: "mods", get label() { return t("shell.serverNav.mods"); }, tabs: [
    item("shell.serverNav.mods", "mods"),
    item("shell.serverNav.configs", "configs"),
    item("shell.serverNav.content", "admin/content"),
    item("shell.serverNav.items", "admin/items"),
    item("shell.serverNav.gameData", "admin/gamedata"),
    item("shell.serverNav.modpack", "modpack"),
  ] },
  { id: "backups", get label() { return t("shell.serverNav.backups"); }, tabs: [
    item("shell.serverNav.backups", "backups"),
    item("shell.serverNav.saves", "admin/saves"),
  ] },
  { id: "settings", get label() { return t("shell.serverNav.settings"); }, tabs: [item("shell.serverNav.settings", "settings")] },
];

const matches = (rest: string, to: string) => rest === to || rest.startsWith(to + "/");

/** The section and page for a path inside a server ("admin/items" → Mods, Items); null for an unknown path. */
export function serverPlace(rest: string): { section: ServerSection; tab: ServerNavItem } | null {
  for (const section of SERVER_SECTIONS) {
    const tab = section.tabs.find((i) => matches(rest, i.to));
    if (tab) return { section, tab };
  }
  return null;
}

/** The page entry for a path inside a server ("/servers/lab/admin/items" → Items). */
export function serverNavFor(rest: string): ServerNavItem | null {
  return serverPlace(rest)?.tab ?? null;
}

/** Old admin routes (`/servers/:id/admin/<from>`) merged into the server's own pages (`/servers/:id/<to>`). App.tsx
 *  redirects them in the router itself: an old link never waits for the admin pages' bundle to load. */
export const ADMIN_MERGED: [from: string, to: string][] = [["configs", "configs"], ["mods", "mods"], ["console", "console"]];
