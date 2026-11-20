// Load order and conflicts (DESIGN 3.4): the resolver's order of the pack's Kiln mods (later wins for data) with the
// pack layer forced last, and the data conflicts of the offline analysis (6.1): which mod wins a field today, and
// "Set in the pack" to decide it in the pack layer.
import { useState } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { Json, PackDataConflict, Studio } from "../api/commands";
import { useT } from "../i18n";
import { Badge, Button, Spinner, TextInput, toast } from "../ui";
import { analyze, sk, useAnalysisOf } from "./state";

const short = (v: Json) => { const s = JSON.stringify(v); return s.length > 40 ? s.slice(0, 39) + "…" : s; };
/** A typed value: JSON when it parses (1000, true, "x"), else the text. */
const parse = (s: string): Json => { try { return JSON.parse(s) as Json; } catch { return s; } };

export default function OrderTab({ studio: st }: { studio: Studio }) {
  const t = useT();
  const order = useQuery({ queryKey: sk.order(st.source.id), queryFn: () => api.studioOrder(st.source.id), retry: false });
  const an = useAnalysisOf(st);
  const conflicts = an.report?.conflicts ?? [];
  let n = 0;
  return (
    <section className="kst-split" data-testid="studio-order">
      <div>
        <h3 className="kst-h3">{t("studio.order.title")}</h3>
        {order.isLoading ? <Spinner /> : order.error ? <p className="kst-p kst-warn-text">{api.toKilnError(order.error).message}</p> : (
          <ol className="kst-order">
            {order.data!.kiln.map((r) => (
              <li key={r.id} className={(r.layer ? "kst-layer" : "") + (r.loads ? "" : " kst-off")} data-testid={"studio-order-" + r.id}>
                <span className="kst-num">{r.loads ? ++n : "–"}</span>
                <span>{r.layer ? "» " : ""}{r.name} <span className="kst-dim kst-mono">{r.id}</span>{r.layer && <span className="kst-dim"> {t("studio.order.always")}</span>}</span>
                {r.reason && <span className="kst-dim">{r.reason}</span>}
              </li>
            ))}
          </ol>
        )}
        {(order.data?.late.length ?? 0) > 0 && <p className="kst-p kst-dim">{t("studio.order.late", { list: order.data!.late.join(", ") })}</p>}
        {order.data?.warnings.map((w) => <p key={w} className="kst-p kst-warn-text">{w}</p>)}
      </div>
      <div>
        <div className="kst-row kst-between">
          <h3 className="kst-h3">{t("studio.order.conflicts")}</h3>
          <Button size="s" variant="ghost" loading={an.running} onClick={() => void analyze(st.source.id, st.source.updated)} data-testid="studio-recheck">{t("studio.recheck")}</Button>
        </div>
        {an.running && !an.report ? <Spinner /> : an.error ? <p className="kst-p kst-warn-text">{an.error}</p>
          : conflicts.length === 0 ? <p className="kst-p kst-dim" data-testid="studio-no-conflicts">{t("studio.order.none")}</p> : (
            <ul className="kst-conflicts">{conflicts.map((c, i) => <ConflictRow key={i} studio={st} c={c} />)}</ul>
          )}
        {an.report && !an.report.catalog && <p className="kst-p kst-dim">{t("studio.order.noCatalog")}</p>}
      </div>
    </section>
  );
}

function ConflictRow({ studio: st, c }: { studio: Studio; c: PackDataConflict }) {
  const t = useT();
  const qc = useQueryClient();
  const [value, setValue] = useState("");
  const [busy, setBusy] = useState(false);
  const layer = "pack." + st.source.id;
  const selector = c.later.selector;
  const set = async () => {
    setBusy(true);
    try {
      await api.studioLayerSet(st.source.id, c.section, selector, c.field, parse(value.trim()));
      void qc.invalidateQueries({ queryKey: sk.layer(st.source.id) });
      const next = await api.studioGet(st.source.id);
      qc.setQueryData(sk.get(st.source.id), next);
      await analyze(st.source.id, next.source.updated);
      toast.ok(t("studio.order.setDone", { field: c.field }));
    } catch (e) { toast.error(e); } finally { setBusy(false); }
  };
  return (
    <li className="kst-conflict" data-testid={"studio-conflict-" + c.section + "-" + c.field}>
      <div className="kst-mono">{c.section} {selector}.{c.field}{c.conditional && <Badge tone="warn">{t("studio.order.maybe")}</Badge>}</div>
      <div className="kst-dim">{c.earlier.mod} {short(c.earlier.value)}</div>
      <div>{c.later.mod} {short(c.later.value)} <span className="kst-win">{t("studio.order.wins")}</span></div>
      {c.later.mod !== layer && (
        <form className="kst-row" onSubmit={(e) => { e.preventDefault(); void set(); }}>
          <span className="kst-dim">{t("studio.order.setIn")}</span>
          <TextInput value={value} onChange={(e) => setValue(e.target.value)} placeholder={short(c.later.value)} aria-label={t("studio.order.setAria", { field: c.field })} data-testid="studio-conflict-value" />
          <Button size="s" type="submit" loading={busy} disabled={!value.trim()} data-testid="studio-conflict-set">{t("studio.order.set")}</Button>
        </form>
      )}
    </li>
  );
}
