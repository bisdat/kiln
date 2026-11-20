// Mods (DESIGN 3.1): the profile's mods as the pack carries them, each the exact Library version (pinned by kdh1);
// required, optional on or optional off (players switch optional ones in the preview), which sides need it, and where
// it comes from (the analysis, so the Credits step is mostly done already). Add from the Library or a file; remove.
import { useState } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { GameId, ImportReport, LibraryRef, PackEnv, Studio, StudioMod } from "../api/commands";
import { useT } from "../i18n";
import { Badge, Button, Modal, Select, WellInput, pickPaths, runTask, toast } from "../ui";
import { matchWord, refreshStudio, useAnalysisOf, useSaveSource } from "./state";

type Dep = "required" | "on" | "off";
type Sides = "both" | "client" | "server";
const depOf = (m: StudioMod): Dep => (m.dep === "required" ? "required" : m.default === false ? "off" : "on");
const sidesOf = (e: PackEnv): Sides => (e.server === "unsupported" ? "client" : e.client === "unsupported" ? "server" : "both");
const envOf = (s: Sides): PackEnv => s === "client" ? { client: "required", server: "unsupported" } : s === "server" ? { client: "unsupported", server: "required" } : { client: "required", server: "required" };

export default function ModsTab({ studio: st, gameId }: { studio: Studio; gameId: GameId | null }) {
  const t = useT();
  const qc = useQueryClient();
  const save = useSaveSource(st.source.id);
  const an = useAnalysisOf(st);
  const [adding, setAdding] = useState(false);
  const credit = (m: StudioMod) => an.report?.credits.find((c) => c.kind === m.kind && c.id === m.id);
  const setChoice = (m: StudioMod, dep: Dep, sides: Sides) => {
    const mods = { ...st.source.mods };
    mods[m.id] = { dep: dep === "required" ? "required" : "optional", ...(dep === "required" ? {} : { default: dep === "on" }), ...(sides === "both" ? {} : { env: envOf(sides) }) };
    save.mutate({ ...st.source, mods }, { onError: (e) => toast.error(e) });
  };
  const remove = async (m: StudioMod) => {
    try {
      const p = await api.profileGet(st.source.profile);
      const list = (p.mods[m.kind] ?? []).filter((x) => x.id !== m.id);
      await api.profileSave({ ...p, mods: { ...p.mods, [m.kind]: list } });
      refreshStudio(qc, st.source.id);
    } catch (e) { toast.error(e, t("studio.mods.removeFailed", { name: m.name })); }
  };
  const addFile = async () => {
    if (!gameId) return;
    const paths = await pickPaths({ multiple: true, title: t("studio.mods.pickFile") });
    if (!paths?.length) return;
    try {
      const r = await runTask<ImportReport>(() => api.libraryImport(paths), { failed: t("studio.mods.addFailed") });
      const refs = [...r.imported, ...r.existing];
      if (refs.length) await api.libraryAddToProfile(gameId, st.source.profile, refs);
      refreshStudio(qc, st.source.id);
    } catch { /* toasted */ }
  };
  return (
    <section className="kst-section" data-testid="studio-mods">
      <div className="kst-row kst-between">
        <p className="kst-p">{t("studio.mods.lead", { profile: st.profileName ?? st.source.profile })}</p>
        <div className="kst-row">
          <Button onClick={() => setAdding(true)} disabled={!gameId} data-testid="studio-add-library">{t("studio.mods.addLibrary")}</Button>
          <Button onClick={() => void addFile()} disabled={!gameId} data-testid="studio-add-file">{t("studio.mods.addFile")}</Button>
        </div>
      </div>
      {st.mods.length === 0 ? <p className="kst-p kst-dim" data-testid="studio-mods-empty">{t("studio.mods.empty")}</p> : (
        <table className="kst-table">
          <thead><tr><th>{t("studio.mods.mod")}</th><th>{t("studio.mods.version")}</th><th>{t("studio.mods.need")}</th><th>{t("studio.mods.sides")}</th><th>{t("studio.mods.source")}</th><th /></tr></thead>
          <tbody>
            {st.mods.map((m) => {
              const c = credit(m);
              const own = m.credit ?? (c?.author ? { author: c.author, source: c.source ?? "" } : null);
              return (
                <tr key={m.kind + ":" + m.id} data-testid={"studio-mod-" + m.id}>
                  <td><span className="kst-mod-name">{m.name}</span> <Badge>{m.kind}</Badge></td>
                  <td className="kst-mono">{m.version}</td>
                  <td>
                    <Select value={depOf(m)} aria-label={t("studio.mods.needAria", { name: m.name })} data-testid={"studio-dep-" + m.id}
                      onChange={(v) => setChoice(m, v as Dep, sidesOf(m.env))}
                      options={[{ value: "required", label: t("studio.mods.required") }, { value: "on", label: t("studio.mods.optionalOn") }, { value: "off", label: t("studio.mods.optionalOff") }]} />
                  </td>
                  <td>
                    <Select value={sidesOf(m.env)} aria-label={t("studio.mods.sidesAria", { name: m.name })} data-testid={"studio-sides-" + m.id}
                      onChange={(v) => setChoice(m, depOf(m), v as Sides)}
                      options={[{ value: "both", label: t("studio.mods.both") }, { value: "client", label: t("studio.mods.client") }, { value: "server", label: t("studio.mods.server") }]} />
                  </td>
                  <td className="kst-source">{own ? <span title={own.source}>{own.author}{c ? <span className="kst-dim"> · {t(matchWord(c.match))}</span> : null}</span>
                    : an.running ? <span className="kst-dim">{t("studio.mods.checking")}</span> : <span className="kst-warn-text">{t("studio.mods.unknown")}</span>}</td>
                  <td><Button size="s" variant="ghost" onClick={() => void remove(m)} data-testid={"studio-remove-" + m.id}>{t("common.remove")}</Button></td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
      {adding && gameId && <AddFromLibrary studio={st} gameId={gameId} onClose={() => setAdding(false)} />}
    </section>
  );
}

/** "Add from my Library": Library entries the profile does not list yet (newest version of each). */
function AddFromLibrary({ studio: st, gameId, onClose }: { studio: Studio; gameId: GameId; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const lib = useQuery({ queryKey: ["library_list"], queryFn: () => api.libraryList() });
  const [filter, setFilter] = useState("");
  const [picked, setPicked] = useState<Record<string, LibraryRef>>({});
  const [busy, setBusy] = useState(false);
  const have = new Set(st.mods.map((m) => m.kind + ":" + m.id));
  const rows = (lib.data ?? []).filter((e) => !have.has(e.kind + ":" + e.id) && !e.id.startsWith("pack.") && !e.id.startsWith("local.")
    && (!filter || (e.name + " " + e.id).toLowerCase().includes(filter.toLowerCase())));
  const add = async () => {
    setBusy(true);
    try {
      await api.libraryAddToProfile(gameId, st.source.profile, Object.values(picked));
      refreshStudio(qc, st.source.id);
      onClose();
    } catch (e) { toast.error(e, t("studio.mods.addFailed")); } finally { setBusy(false); }
  };
  return (
    <Modal open onClose={onClose} title={t("studio.mods.addLibrary")} width={600}
      footer={<><Button variant="ghost" onClick={onClose}>{t("common.cancel")}</Button>
        <Button variant="primary" loading={busy} disabled={!Object.keys(picked).length} onClick={() => void add()} data-testid="studio-add-confirm">{t("studio.mods.addN", { count: Object.keys(picked).length })}</Button></>}>
      <WellInput label={t("studio.mods.filter")} value={filter} onChange={(e) => setFilter(e.target.value)} data-esc-clears />
      <ul className="kst-pick" data-testid="studio-library">
        {rows.map((e) => {
          const v = e.versions[0];
          const key = e.kind + ":" + e.id;
          return (
            <li key={key}>
              <label className="kst-tick">
                <input type="checkbox" className="k-check" checked={!!picked[key]} data-testid={"studio-lib-" + e.id}
                  onChange={(ev) => setPicked((p) => { const n = { ...p }; if (ev.target.checked) n[key] = { kind: e.kind, id: e.id, version: v.version, hash: v.hash }; else delete n[key]; return n; })} />
                <span>{e.name} <span className="kst-dim kst-mono">{v.version}</span> <Badge>{e.kind}</Badge></span>
              </label>
            </li>
          );
        })}
        {!rows.length && <li className="kst-dim">{t("studio.mods.libraryEmpty")}</li>}
      </ul>
    </Modal>
  );
}
