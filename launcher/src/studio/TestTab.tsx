// Test this pack (DESIGN 3.5): the pack is built and installed as an instance of its own ("<name> (test)"), so the
// creator plays exactly what players get; Kiln backs up the saves first. Start it with Play, use a new world to keep
// your own saves clean. What the game logged (data conflicts, errors) comes back here. Server packs test through the
// lab path (a server of the launcher: Publish, "Put on a server").
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import * as api from "../api/commands";
import type { GameId, PackInstallResult, Studio } from "../api/commands";
import { useT } from "../i18n";
import { usePlayControl } from "../shell/play";
import { Banner, Button, runTask, toast } from "../ui";
import { refreshStudio, sk } from "./state";

export default function TestTab({ studio: st, gameId }: { studio: Studio; gameId: GameId | null }) {
  const t = useT();
  const qc = useQueryClient();
  const ctl = usePlayControl();
  const [busy, setBusy] = useState(false);
  const id = st.source.id;
  const testing = st.source.testProfile ?? null;
  const report = useQuery({ queryKey: ["studio_test_report", id, testing], queryFn: () => api.studioTestReport(id, gameId), enabled: !!testing, refetchInterval: 15_000 });
  const make = async () => {
    setBusy(true);
    try {
      const r = await runTask<PackInstallResult>(() => api.studioTest(id, gameId), { failed: t("studio.test.failed") });
      refreshStudio(qc, id);
      void qc.invalidateQueries({ queryKey: ["studio_test_report", id] });
      toast.ok(t("studio.test.ready", { name: r.profile.name }), t("studio.test.readyDetail"));
    } catch { /* toasted */ } finally { setBusy(false); }
  };
  const end = async () => {
    try { await api.studioTestEnd(id); refreshStudio(qc, id); qc.setQueryData(sk.get(id), await api.studioGet(id)); } catch (e) { toast.error(e); }
  };
  const r = report.data;
  return (
    <section className="kst-section" data-testid="studio-test">
      <p className="kst-p">{t("studio.test.lead")}</p>
      <ul className="kst-bullets">
        <li>{t("studio.test.backup")}</li>
        <li>{t("studio.test.world")}</li>
        <li>{t("studio.test.server")}</li>
      </ul>
      <div className="kst-row">
        <Button variant="primary" loading={busy} disabled={!gameId} onClick={() => void make()} data-testid="studio-test-make">{testing ? t("studio.test.again") : t("studio.test.make")}</Button>
        {testing && <Button disabled={!ctl.canPlay} onClick={() => ctl.play("profile", testing)} data-testid="studio-test-play">{t("studio.test.play")}</Button>}
        {testing && <Button variant="ghost" onClick={() => void end()} data-testid="studio-test-end">{t("studio.test.end")}</Button>}
      </div>
      {!gameId && <p className="kst-p kst-dim">{t("studio.test.noGame")}</p>}
      {testing && (
        <div className="kst-report" data-testid="studio-test-report">
          <h3 className="kst-h3">{t("studio.test.reported")}</h3>
          {!r?.ran ? <p className="kst-p kst-dim">{t("studio.test.notYet")}</p> : r.conflicts.length + r.problems.length === 0
            ? <Banner tone="ok">{t("studio.test.clean")}</Banner> : (
              <>
                {r.conflicts.length > 0 && <><p className="kst-label">{t("studio.test.conflicts", { count: r.conflicts.length })}</p>
                  <pre className="kst-log">{r.conflicts.join("\n")}</pre></>}
                {r.problems.length > 0 && <><p className="kst-label">{t("studio.test.problems", { count: r.problems.length })}</p>
                  <pre className="kst-log">{r.problems.join("\n")}</pre></>}
              </>
            )}
        </div>
      )}
    </section>
  );
}
