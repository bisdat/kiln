// Pack Studio (DESIGN 3): the creator's tool for Crucible packs, desktop only (the Deck installs Studio-made packs like any
// other). /studio lists the packs and starts a new one from a profile (or empty); /studio/:packId/:tab is one pack with
// its tabs: Mods, Settings, Data, Load order, Test, Credits, Publish.
import { lazy, Suspense, useState } from "react";
import { Navigate, NavLink, useNavigate, useParams } from "react-router-dom";
import { useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { ProfileId, Studio } from "../api/commands";
import { useGame, useProfiles } from "../api/hooks";
import { useT } from "../i18n";
import { Button, ConfirmDanger, EmptyState, Field, Page, PageHeader, Select, Spinner, TextInput, fmtAgo, toast } from "../ui";
import { useCrucibleStatus } from "../crucible/state";
import COVER from "../assets/crucible/cover-forge.webp";
import EMPTY from "../assets/crucible/empty.webp";
import MARK from "../assets/crucible/mark-64.png";
import { TABS, refreshStudio, sk, useStudio, useStudioList, type StudioTab } from "./state";
import ModsTab from "./ModsTab";
import SettingsTab from "./SettingsTab";
import OrderTab from "./OrderTab";
import TestTab from "./TestTab";
import CreditsTab from "./CreditsTab";
import PublishTab from "./PublishTab";
import "./studio.css";

const DataTab = lazy(() => import("./data/DataTab"));

export default function StudioPage() {
  const status = useCrucibleStatus();
  const { packId, tab } = useParams();
  // a deep link waits for Crucible's status: only a known "off" leaves the Studio
  if (status.isLoading) return <Page><Spinner /></Page>;
  if (status.data?.enabled !== true) return <Navigate to="/" replace />;
  if (!packId) return <StudioIndex />;
  if (!tab || !(TABS as readonly string[]).includes(tab)) return <Navigate to={"/studio/" + encodeURIComponent(packId) + "/mods"} replace />;
  return <StudioPack packId={packId} tab={tab as StudioTab} />;
}

function StudioIndex() {
  const t = useT();
  const list = useStudioList();
  const [creating, setCreating] = useState(false);
  const packs = list.data ?? [];
  return (
    <Page wide>
      <PageHeader title={t("studio.title")} sub={t("studio.sub")} meta={<img src={MARK} alt="" width={22} height={22} className="kst-mark" />}
        actions={<Button variant="primary" onClick={() => setCreating(true)} data-testid="studio-new">{t("studio.new")}</Button>} />
      {creating && <NewPack onClose={() => setCreating(false)} />}
      {list.isLoading ? <Spinner /> : packs.length === 0 && !creating ? (
        <div className="kst-empty" data-testid="studio-empty">
          <img src={EMPTY} alt="" />
          <EmptyState title={t("studio.empty.title")} hint={t("studio.empty.hint")}
            action={<Button variant="primary" onClick={() => setCreating(true)}>{t("studio.new")}</Button>} />
        </div>
      ) : (
        <ul className="kst-packs" data-testid="studio-packs">
          {packs.map((p) => (
            <li key={p.id}>
              <NavLink to={"/studio/" + encodeURIComponent(p.id) + "/mods"} className="kst-pack" data-testid={"studio-pack-" + p.id}>
                <span className="kst-pack-cover" style={{ backgroundImage: "url(" + COVER + ")" }} aria-hidden />
                <span className="kst-pack-text">
                  <span className="kst-pack-name">{p.name}</span>
                  <span className="kst-pack-sub">{t("studio.packLine", { version: p.version, count: p.mods, ago: fmtAgo(p.updated) })}</span>
                  {p.published && <span className="kst-pack-pub">{p.published.visibility === "public" ? t("studio.state.public") : t("studio.state.link")}{p.published.code ? " · " + p.published.code : ""}</span>}
                </span>
              </NavLink>
            </li>
          ))}
        </ul>
      )}
    </Page>
  );
}

/** "New pack from: [profile] or [Empty]". */
function NewPack({ onClose }: { onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const nav = useNavigate();
  const profiles = useProfiles();
  const [name, setName] = useState("");
  const [from, setFrom] = useState<string>("");
  const [busy, setBusy] = useState(false);
  const list = (profiles.data ?? []).filter((p) => !p.name.endsWith("(test)"));
  const create = async () => {
    setBusy(true);
    try {
      const st = await api.studioCreate({ name: name.trim() || list.find((p) => p.id === from)?.name || "", from: from || null });
      void qc.invalidateQueries({ queryKey: sk.list });
      void qc.invalidateQueries({ queryKey: ["profile_list"] });
      nav("/studio/" + encodeURIComponent(st.source.id) + "/mods");
    } catch (e) { toast.error(e, t("studio.newFailed")); } finally { setBusy(false); }
  };
  const count = (pid: ProfileId) => { const p = list.find((x) => x.id === pid); return p ? Object.values(p.counts).reduce((a, b) => a + b, 0) : undefined; };
  return (
    <form className="kst-new" data-testid="studio-new-form" onSubmit={(e) => { e.preventDefault(); void create(); }}>
      <Field label={t("studio.newFrom")}>
        <Select value={from} onChange={setFrom} data-testid="studio-new-from"
          options={[{ value: "", label: t("studio.newEmpty") }, ...list.map((p) => ({ value: p.id, label: count(p.id) !== undefined ? t("studio.newProfile", { name: p.name, count: count(p.id)! }) : p.name }))]} />
      </Field>
      <Field label={t("studio.newName")}>
        <TextInput value={name} onChange={(e) => setName(e.target.value)} placeholder={list.find((p) => p.id === from)?.name ?? t("studio.newNamePh")} data-testid="studio-new-name" />
      </Field>
      <div className="kst-row">
        <Button variant="primary" type="submit" loading={busy} disabled={!name.trim() && !from} data-testid="studio-create">{t("studio.create")}</Button>
        <Button variant="ghost" onClick={onClose}>{t("common.cancel")}</Button>
      </div>
    </form>
  );
}

function StudioPack({ packId, tab }: { packId: string; tab: StudioTab }) {
  const t = useT();
  const q = useStudio(packId);
  const { game } = useGame();
  const qc = useQueryClient();
  const nav = useNavigate();
  const [del, setDel] = useState(false);
  if (q.isLoading) return <Page><Spinner /></Page>;
  if (q.error || !q.data) return <Page><EmptyState title={t("studio.missing")} hint={api.toKilnError(q.error).message} action={<Button onClick={() => nav("/studio")}>{t("studio.back")}</Button>} /></Page>;
  const st: Studio = q.data;
  const s = st.source;
  const label: Record<StudioTab, string> = {
    mods: t("studio.tab.mods"), settings: t("studio.tab.settings"), data: t("studio.tab.data"), order: t("studio.tab.order"),
    test: t("studio.tab.test"), credits: t("studio.tab.credits"), publish: t("studio.tab.publish"),
  };
  const state = s.published ? (s.published.visibility === "public" ? t("studio.state.public") : t("studio.state.link")) : t("studio.state.draft");
  return (
    <Page wide>
      <PageHeader title={s.name} meta={<span className="kst-meta" data-testid="studio-state">{state}</span>}
        sub={st.profileName ? t("studio.packSub", { version: s.version, profile: st.profileName, count: st.mods.length }) : t("studio.profileGone")}
        actions={<>
          <Button variant="ghost" onClick={() => nav("/studio")}>{t("studio.allPacks")}</Button>
          <Button variant="ghost" onClick={() => setDel(true)} data-testid="studio-delete">{t("common.delete")}</Button>
        </>} />
      <nav className="k-tabs kst-tabs" aria-label={t("studio.tabsAria")} data-testid="studio-tabs">
        {TABS.map((id) => (
          <NavLink key={id} to={"/studio/" + encodeURIComponent(packId) + "/" + id} className={({ isActive }) => "k-tab" + (isActive ? " active" : "")} data-testid={"studio-tab-" + id}>
            {label[id]}
          </NavLink>
        ))}
      </nav>
      <div className="kst-body">
        {tab === "mods" && <ModsTab studio={st} gameId={game?.id ?? null} />}
        {tab === "settings" && <SettingsTab studio={st} gameId={game?.id ?? null} />}
        {tab === "data" && <Suspense fallback={<Spinner />}><DataTab studio={st} gameId={game?.id ?? null} /></Suspense>}
        {tab === "order" && <OrderTab studio={st} />}
        {tab === "test" && <TestTab studio={st} gameId={game?.id ?? null} />}
        {tab === "credits" && <CreditsTab studio={st} />}
        {tab === "publish" && <PublishTab studio={st} gameId={game?.id ?? null} />}
      </div>
      {del && (
        <ConfirmDanger open title={t("studio.deleteTitle", { name: s.name })} onClose={() => setDel(false)} action={t("common.delete")} word={s.id} relaxable
          message={t("studio.deleteText")}
          onConfirm={async () => {
            try { await api.studioDelete(packId); refreshStudio(qc, packId); nav("/studio"); } catch (e) { toast.error(e); throw e; }
          }} />
      )}
    </Page>
  );
}
