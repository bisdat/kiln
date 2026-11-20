// Credits (DESIGN 3.6): attribution for every mod, prefilled from the pack analysis' identification (Kiln's own mods,
// earlier Crucible credits, GitHub, Nexus, the manifest); the creator confirms or fills each row. Public packs need an
// author and an https link for every mod; link-only shares need none.
import { useEffect, useMemo, useState } from "react";
import { useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { PackCredit, PackCreditRow, Studio } from "../api/commands";
import { useT } from "../i18n";
import { Badge, Button, Spinner, TextInput, toast } from "../ui";
import { analyze, confidenceWord, matchWord, sk, useAnalysisOf } from "./state";

const https = (s: string) => /^https:\/\/[^\s]+\.[^\s]+/.test(s.trim());

export default function CreditsTab({ studio: st }: { studio: Studio }) {
  const t = useT();
  const qc = useQueryClient();
  const an = useAnalysisOf(st);
  const [rows, setRows] = useState<Record<string, PackCredit>>({});
  // rows the creator typed in keep their text when the analysis or the pack is read again
  const [touched, setTouched] = useState<Record<string, true>>({});
  const [busy, setBusy] = useState(false);
  const edit = (id: string, c: PackCredit) => { setTouched((x) => ({ ...x, [id]: true })); setRows((x) => ({ ...x, [id]: c })); };
  const found = useMemo(() => new Map<string, PackCreditRow>((an.report?.credits ?? []).map((c) => [c.id, c])), [an.report]);
  // the saved credit, else the analysis' prefill
  useEffect(() => {
    setRows((prev) => {
      const next: Record<string, PackCredit> = {};
      for (const m of st.mods) {
        if (touched[m.id] && prev[m.id]) { next[m.id] = prev[m.id]; continue; }
        const own = st.credits[m.id];
        const c = found.get(m.id);
        next[m.id] = own ?? { author: c?.author ?? "", source: c?.source ?? "", ...(c?.match ? { match: c.match } : {}) };
      }
      return next;
    });
  }, [st.mods, st.credits, found, touched]);
  const missing = st.mods.filter((m) => !(rows[m.id]?.author?.trim() && https(rows[m.id]?.source ?? "")));
  const changed = st.mods.some((m) => JSON.stringify(rows[m.id] ?? null) !== JSON.stringify(st.credits[m.id] ?? null) && (rows[m.id]?.author || rows[m.id]?.source));
  const save = async () => {
    setBusy(true);
    try {
      const clean = Object.fromEntries(Object.entries(rows).map(([id, c]) => {
        const f = found.get(id);
        const same = f && c.author === (f.author ?? "") && c.source === (f.source ?? "");
        return [id, { author: c.author.trim(), source: c.source.trim(), match: same ? f!.match : "manual" }];
      }).filter(([, c]) => (c as PackCredit).author || (c as PackCredit).source));
      const next = await api.studioCreditsSet(st.source.id, clean);
      qc.setQueryData(sk.get(st.source.id), next);
      setTouched({});
      toast.ok(t("studio.credits.saved"));
    } catch (e) { toast.error(e, t("studio.credits.saveFailed")); } finally { setBusy(false); }
  };
  return (
    <section className="kst-section" data-testid="studio-credits">
      <div className="kst-row kst-between">
        <p className="kst-p">{t("studio.credits.lead")}</p>
        <Button size="s" variant="ghost" loading={an.running} onClick={() => void analyze(st.source.id, st.revision)}>{t("studio.recheck")}</Button>
      </div>
      {an.running && !an.report && <Spinner />}
      <table className="kst-table kst-credits">
        <thead><tr><th>{t("studio.mods.mod")}</th><th>{t("studio.credits.author")}</th><th>{t("studio.credits.source")}</th><th>{t("studio.credits.matched")}</th></tr></thead>
        <tbody>
          {st.mods.map((m) => {
            const r = rows[m.id] ?? { author: "", source: "" };
            const f = found.get(m.id);
            const bad = !!r.source && !https(r.source);
            const ok = r.author.trim() && https(r.source);
            return (
              <tr key={m.kind + ":" + m.id} data-testid={"studio-credit-" + m.id}>
                <td>{m.name} <span className="kst-dim kst-mono">{m.version}</span></td>
                <td><TextInput value={r.author} placeholder="?" aria-label={t("studio.credits.authorAria", { name: m.name })} data-testid={"studio-author-" + m.id}
                  onChange={(e) => edit(m.id, { ...r, author: e.target.value })} /></td>
                <td><TextInput value={r.source} placeholder="https://" invalid={bad} aria-label={t("studio.credits.sourceAria", { name: m.name })} data-testid={"studio-source-" + m.id}
                  onChange={(e) => edit(m.id, { ...r, source: e.target.value })} /></td>
                <td>{ok && f && r.author === (f.author ?? "") && r.source === (f.source ?? "")
                  ? <span>{t(matchWord(f.match))} {f.confidence !== "certain" && <Badge tone={f.confidence === "conflict" ? "err" : "warn"}>{t(confidenceWord(f.confidence))}</Badge>}</span>
                  : ok ? <span>{t("studio.match.manual")}</span>
                  : <span className="kst-warn-text">{t("studio.credits.notIdentified")}</span>}
                  {f?.flag && <div className="kst-dim">{f.flag}</div>}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <div className="kst-row kst-between">
        <span className={missing.length ? "kst-warn-text" : "kst-ok-text"} data-testid="studio-credits-state">
          {missing.length ? t("studio.credits.missing", { count: missing.length }) : t("studio.credits.complete")}
        </span>
        <Button variant="primary" loading={busy} disabled={!changed} onClick={() => void save()} data-testid="studio-credits-save">{t("studio.credits.save")}</Button>
      </div>
    </section>
  );
}
