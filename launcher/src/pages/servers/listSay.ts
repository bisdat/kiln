// The Servers list's sentence (ServersPage.tsx' header): what runs, what is busy, what has a Kiln update, in one line.
import type { TFunction } from "../../i18n";
import type { ServerRow } from "./serverListCells";

/** "3 servers. Ember Isles is running with 3 players; Hollow Lab has a Kiln update." */
export function listSentence(rows: ServerRow[], t: TFunction): string {
  const count = t("servers.list.say.count", { count: rows.length });
  const parts: string[] = [];
  const busy = rows.filter((r) => r.flow?.state === "running");
  if (busy.length === 1) {
    const r = busy[0], name = r.srv.name;
    switch (r.flow!.kind) {
      case "start": parts.push(t("servers.list.say.busyStart", { name })); break;
      case "stop": parts.push(t("servers.list.say.busyStop", { name })); break;
      case "restart": parts.push(t("servers.list.say.busyRestart", { name })); break;
      case "update": parts.push(t("servers.list.say.busyUpdate", { name })); break;
      case "deploy": parts.push(t("servers.list.say.busyDeploy", { name })); break;
      case "create": parts.push(t("servers.list.say.busyCreate", { name })); break;
      case "backup": parts.push(t("servers.list.say.busyBackup", { name })); break;
    }
  } else if (busy.length > 1) parts.push(t("servers.list.say.busyMany", { count: busy.length }));
  const up = rows.filter((r) => r.flow?.state !== "running" && r.st?.state === "running");
  const players = (r: ServerRow) => r.st?.players ?? 0;
  const withPlayers = up.filter((r) => players(r) > 0);
  if (up.length === 1 || withPlayers.length === 1) {
    const r = withPlayers[0] ?? up[0];
    parts.push(players(r) ? t("servers.list.say.runningPlayers", { name: r.srv.name, count: players(r) }) : t("servers.list.say.runningNobody", { name: r.srv.name }));
  } else if (up.length > 1) {
    const all = up.reduce((n, r) => n + players(r), 0);
    parts.push(all ? t("servers.list.say.runningMany", { servers: up.length, count: all }) : t("servers.list.say.runningManyNobody", { servers: up.length }));
  } else if (!busy.length && rows.length && rows.every((r) => r.st)) parts.push(t("servers.list.say.noneRunning"));
  // a server whose flow runs is said once, by its flow
  const upd = rows.filter((r) => r.update && r.flow?.state !== "running");
  if (upd.length === 1) parts.push(t("servers.list.say.updateOne", { name: upd[0].srv.name }));
  else if (upd.length > 1) parts.push(t("servers.list.say.updateMany", { count: upd.length }));
  const rest = parts.length === 1 ? t("servers.list.say.join1", { a: parts[0] }) : parts.length === 2 ? t("servers.list.say.join2", { a: parts[0], b: parts[1] })
    : parts.length > 2 ? t("servers.list.say.join3", { a: parts[0], b: parts[1], c: parts.slice(2).join("; ") }) : "";
  return rest ? t("servers.list.say.full", { count, rest }) : count;
}
