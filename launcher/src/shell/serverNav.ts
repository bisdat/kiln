// One grouped list per server (it replaces the server tabs and the admin tabs): Run, World, Data, Files. Each entry is a
// route under /servers/:id/. Configs, Mods and the console are merged pages (live while the server runs, the files
// while it is stopped).

export interface ServerNavItem {
  label: string;
  /** Route under /servers/:id/. */
  to: string;
}

export const SERVER_NAV: { group: string; items: ServerNavItem[] }[] = [
  { group: "Run", items: [
    { label: "Overview", to: "overview" },
    { label: "Logs", to: "logs" },
    { label: "Tick stats", to: "ticks" },
  ] },
  { group: "World", items: [
    { label: "Players", to: "admin/players" },
    { label: "Map", to: "admin/map" },
    { label: "Zones", to: "admin/zones" },
    { label: "Spawn", to: "admin/spawn" },
    { label: "Creatures", to: "admin/creatures" },
    { label: "Combat", to: "admin/combat" },
    { label: "World", to: "admin/world" },
  ] },
  { group: "Data", items: [
    { label: "Items", to: "admin/items" },
    { label: "Game data", to: "admin/gamedata" },
    { label: "Content", to: "admin/content" },
    { label: "Permissions", to: "admin/permissions" },
  ] },
  { group: "Files", items: [
    { label: "Settings", to: "settings" },
    { label: "Configs", to: "configs" },
    { label: "Mods", to: "mods" },
    { label: "Backups", to: "backups" },
    { label: "Saves", to: "admin/saves" },
    { label: "Modpack", to: "modpack" },
    { label: "Crashes", to: "admin/crashes" },
  ] },
];

/** The list entry for a path inside a server ("/servers/lab/admin/items" → Items). */
/** Old admin routes (`/servers/:id/admin/<from>`) merged into the server's own pages (`/servers/:id/<to>`). App.tsx
 *  redirects them in the router itself: an old link never waits for the admin pages' bundle to load. */
export const ADMIN_MERGED: [from: string, to: string][] = [["configs", "configs"], ["mods", "mods"], ["console", "overview"]];

export function serverNavFor(rest: string): ServerNavItem | null {
  const all = SERVER_NAV.flatMap((g) => g.items);
  return all.find((i) => rest === i.to || rest.startsWith(i.to + "/")) ?? null;
}
