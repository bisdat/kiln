// "Recent" on the Servers list: the launcher-side activity log (activityLog.ts), newest first: when, which server, and
// what happened in one sentence.
import { Card } from "../../ui";
import { formatLocale, useT } from "../../i18n";
import { activitySentence, fmtClock, type ActivityEntry } from "./activityLog";

export function RecentActivity({ entries, names, now }: { entries: ActivityEntry[]; names: Record<string, string>; now: number }) {
  const t = useT();
  return (
    <Card className="k-recent" title={t("servers.recent.title")} sub={t("servers.recent.hint")}>
      {entries.length ? (
        <ol className="k-recent-list" aria-label={t("servers.recent.label")}>
          {entries.map((e, i) => (
            <li key={`${e.at}-${e.serverId}-${i}`} className={e.state === "failed" ? "err" : undefined}>
              <time className="k-recent-at k-mono" dateTime={new Date(e.at).toISOString()}
                title={new Date(e.at).toLocaleString(formatLocale(), { dateStyle: "medium", timeStyle: "short" })}>{fmtClock(e.at, now)}</time>
              <span className="k-recent-who">{names[e.serverId] ?? e.name ?? e.serverId}</span>
              <span className="k-recent-what">{activitySentence(e, t)}</span>
            </li>
          ))}
        </ol>
      ) : <p className="k-empty-title" style={{ margin: 0 }}>{t("servers.recent.none")}</p>}
    </Card>
  );
}
