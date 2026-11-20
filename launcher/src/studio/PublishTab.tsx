// Publish (DESIGN 3.6): the next version to Crucible through the launcher's Crucible client: version, what changed, the
// game build it was tested with, link-only or public, the checks of the pack analysis, and for public packs the
// attestation. An update with unchanged credits is two clicks (this tab, Publish). Earlier versions are listed with
// "Put on a server": the server pack from that version (its server part on one of the launcher's servers and the
// player modpack built from it), and Save as file.
import { useEffect, useState } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { GameId, PackServerInstallResult, Studio, StudioPublishResult, StudioVisibility } from "../api/commands";
import { useGame } from "../api/hooks";
import { useT } from "../i18n";
import { useCommunityUi } from "../community/state";
import { copyText, crucibleError, shortLink, useCrucibleStatus } from "../crucible/state";
import { Banner, Button, Field, Modal, Select, TextArea, TextInput, fmtAgo, pickPaths, runTask, toast } from "../ui";
import { analyze, refreshStudio, sk, toneWord, useAnalysisOf } from "./state";

export default function PublishTab({ studio: st, gameId }: { studio: Studio; gameId: GameId | null }) {
  const t = useT();
  const qc = useQueryClient();
  const status = useCrucibleStatus();
  const { game } = useGame();
  const s = st.source;
  const an = useAnalysisOf(st);
  const [version, setVersion] = useState(s.version);
  const [changes, setChanges] = useState(s.changes ?? "");
  const [summary, setSummary] = useState(s.summary ?? "");
  // what the creator asked for last (a public pack Crucible did not take yet stays asked for: Make public)
  const [vis, setVis] = useState<StudioVisibility>(s.visibility ?? s.published?.visibility ?? "link");
  const [gone, setGone] = useState(false);
  const [visBusy, setVisBusy] = useState(false);
  const [visNeeds, setVisNeeds] = useState<string[]>([]);
  // a pack published public before was attested then: unchanged credits need no new tick (two clicks to update)
  const [attest, setAttest] = useState(s.published?.visibility === "public");
  const build = game?.buildId ? Number(game.buildId) : null;
  const [tested, setTested] = useState(true);
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState<StudioPublishResult | null>(null);
  const [server, setServer] = useState<string | null>(null);
  useEffect(() => { setVersion(s.version); }, [s.version]);
  const request = { version, changes: changes.trim() || null, visibility: vis, attest, gameBuilds: tested && build ? [build] : [], fresh: gone };
  const needs = useQuery({ queryKey: ["studio_publish_check", s.id, request, s.updated], queryFn: () => api.studioPublishCheck(s.id, request), retry: false });
  const summaryDirty = (summary.trim() || null) !== (s.summary ?? null);
  const publish = async () => {
    setBusy(true);
    setDone(null);
    try {
      if (summaryDirty) qc.setQueryData(sk.get(s.id), await api.studioSave(s.id, { ...s, summary: summary.trim() || null }));
      const r = await runTask<StudioPublishResult>(() => api.studioPublish(s.id, request, gameId), { quiet: true });
      setDone(r);
      setGone(false);
      setVisNeeds(r.needs);
      refreshStudio(qc, s.id);
      const copied = await copyText(r.pack.url);
      toast.ok(r.public ? t("studio.publish.donePublic", { version: r.version.version }) : t("studio.publish.doneLink", { version: r.version.version }),
        (copied ? t("studio.publish.copied") + " " : "") + shortLink(r.pack.url));
    } catch (e) {
      const k = api.toKilnError(e);
      if (k.code === "auth_failed") useCommunityUi.getState().openSignIn(false);
      // deleted or taken down on Crucible: never quietly a new pack; the creator chooses it
      if ((k.detail as { gone?: boolean } | undefined)?.gone) setGone(true);
      toast.error(crucibleError(k), t("studio.publish.failed"));
    } finally { setBusy(false); }
  };
  const saveFile = async () => {
    const dir = api.isTauri() ? (await pickPaths({ directory: true, title: t("crucible.save.pick") }))?.[0] : (await api.loadMock()).mockPickFolder();
    if (!dir) return;
    try {
      const r = await runTask<api.BuiltPack>(() => api.studioBuild(s.id, dir), { failed: t("crucible.save.failed", { name: s.name }) });
      toast.ok(t("crucible.save.done", { name: s.name }), r.file);
    } catch { /* toasted */ }
  };
  /** Make public / Link only for a published pack, without a new version. */
  const setVisibility = async (to: StudioVisibility) => {
    setVisBusy(true);
    try {
      if (summaryDirty) qc.setQueryData(sk.get(s.id), await api.studioSave(s.id, { ...s, summary: summary.trim() || null }));
      const r = await api.studioSetVisibility(s.id, to, attest);
      setVisNeeds(r.needs);
      refreshStudio(qc, s.id);
      if (r.public) toast.ok(t("studio.publish.isPublic"), shortLink(r.pack.url));
      else if (to === "link") toast.ok(t("studio.publish.isLink"));
    } catch (e) {
      const k = api.toKilnError(e);
      if (k.code === "auth_failed") useCommunityUi.getState().openSignIn(false);
      toast.error(crucibleError(k), t("studio.publish.failed"));
    } finally { setVisBusy(false); }
  };
  const visDiffers = !!s.published && !s.published.versions.every((v) => v.pending) && s.published.visibility !== vis;
  const report = an.report;
  const signedIn = !!status.data?.signedIn;
  const blocking = needs.data ?? [];
  const checks = report ? [
    [t("studio.publish.check.format"), report.findings.some((f) => f.level === "error" && /^(format|zip|index|entry|kdh1|hash)/.test(f.code)) ? "err" : "ok"],
    [t("studio.publish.check.sizes"), report.findings.some((f) => f.code.startsWith("size")) ? "warn" : "ok"],
    [t("studio.publish.check.blocked"), report.mods.some((m) => m.blocked) ? "err" : "ok"],
    [t("studio.publish.check.code", { count: report.findings.filter((f) => f.code.startsWith("code")).length }), report.findings.some((f) => f.code.startsWith("code")) ? "note" : "ok"],
    [t("studio.publish.check.lint", { count: report.summary.warnings }), report.summary.errors ? "err" : report.summary.warnings ? "warn" : "ok"],
  ] as [string, string][] : [];
  return (
    <section className="kst-section" data-testid="studio-publish">
      {!signedIn && (
        <Banner tone="info" title={t("studio.publish.signInTitle")} actions={<Button size="s" variant="primary" onClick={() => useCommunityUi.getState().openSignIn(false)} data-testid="studio-signin">{t("crucible.share.signIn")}</Button>}>
          {t("studio.publish.signIn")}
        </Banner>
      )}
      <div className="kst-form">
        <Field label={t("studio.publish.version")}><TextInput value={version} onChange={(e) => setVersion(e.target.value)} mono data-testid="studio-version" /></Field>
        <Field label={t("studio.publish.changes")}><TextArea value={changes} onChange={(e) => setChanges(e.target.value)} rows={2} placeholder={t("studio.publish.changesPh")} data-testid="studio-changes" /></Field>
        {build && <label className="kst-tick"><input type="checkbox" className="k-check" checked={tested} onChange={(e) => setTested(e.target.checked)} />{t("studio.publish.tested", { build })}</label>}
        <div className="kst-row" role="radiogroup" aria-label={t("studio.publish.visibility")}>
          <span className="kst-label">{t("studio.publish.visibility")}</span>
          <label className="kst-tick"><input type="radio" name="vis" checked={vis === "public"} onChange={() => setVis("public")} data-testid="studio-vis-public" />{t("studio.publish.public")}</label>
          <label className="kst-tick"><input type="radio" name="vis" checked={vis === "link"} onChange={() => setVis("link")} data-testid="studio-vis-link" />{t("studio.publish.link")}</label>
        </div>
        {vis === "public" && (
          <>
            <Field label={t("studio.publish.summary")} hint={t("studio.publish.summaryHint")}>
              <TextInput value={summary} onChange={(e) => setSummary(e.target.value)} maxLength={256} data-testid="studio-summary" />
            </Field>
            <label className="kst-tick"><input type="checkbox" className="k-check" checked={attest} onChange={(e) => setAttest(e.target.checked)} data-testid="studio-attest" />{t("studio.publish.attest")}</label>
          </>
        )}
      </div>
      <div className="kst-checks" data-testid="studio-checks">
        <span className="kst-label">{t("studio.publish.checks")}</span>
        {an.running && !report ? <span className="kst-dim">{t("studio.mods.checking")}</span> : an.error ? <span className="kst-warn-text">{an.error}</span>
          : checks.map(([label, tone]) => <span key={label} className={"kst-check kst-check-" + tone}>{label} [{t(toneWord(tone))}]</span>)}
        <Button size="s" variant="ghost" loading={an.running} onClick={() => void analyze(s.id, st.revision)}>{t("studio.recheck")}</Button>
      </div>
      {blocking.filter((n) => !(summaryDirty && n.includes("summary"))).length > 0 && (
        <ul className="kst-needs" data-testid="studio-needs">{blocking.map((n) => <li key={n}>{n}</li>)}</ul>
      )}
      <div className="kst-row">
        <Button variant="primary" loading={busy} disabled={!signedIn || (blocking.length > 0 && !(summaryDirty && blocking.every((n) => n.includes("summary"))))}
          onClick={() => void publish()} data-testid="studio-publish-go">{gone ? t("studio.publish.goFresh") : t("studio.publish.go")}</Button>
        {visDiffers && (
          <Button loading={visBusy} disabled={!signedIn || (vis === "public" && blocking.some((n) => !n.includes("published already")))}
            onClick={() => void setVisibility(vis)} data-testid="studio-set-visibility">{vis === "public" ? t("studio.publish.makePublic") : t("studio.publish.makeLink")}</Button>
        )}
        <Button variant="ghost" onClick={() => void saveFile()} data-testid="studio-save-file">{t("studio.publish.saveFile")}</Button>
      </div>
      {gone && <Banner tone="warn" title={t("studio.publish.goneTitle")}>{t("studio.publish.gone")}</Banner>}
      {visNeeds.length > 0 && !done?.public && (
        <Banner tone="warn" title={t("studio.publish.notYetPublic")}><ul className="kst-needs" data-testid="studio-vis-needs">{visNeeds.map((n) => <li key={n}>{n}</li>)}</ul></Banner>
      )}
      {done && (
        <Banner tone={done.public || vis === "link" ? "ok" : "warn"} title={done.public ? t("studio.publish.isPublic") : t("studio.publish.isLink")}>
          <span data-testid="studio-published-link">{shortLink(done.pack.url)}</span> · <span className="kcr-code">{done.pack.code}</span>

        </Banner>
      )}
      {s.published && (
        <div className="kst-history" data-testid="studio-history">
          <h3 className="kst-h3">{t("studio.publish.history")}</h3>
          <ul>
            {[...s.published.versions].reverse().filter((v) => !v.pending).map((v) => (
              <li key={v.id} className="kst-row kst-between" data-testid={"studio-version-" + v.version}>
                <span><span className="kst-mono">{v.version}</span> <span className="kst-dim">{v.visibility === "public" ? t("studio.state.public") : t("studio.state.link")} · {fmtAgo(v.published)}</span></span>
                <Button size="s" variant="ghost" disabled={!v.file} onClick={() => setServer(v.version)} data-testid={"studio-server-pack-" + v.version}>{t("studio.publish.toServer")}</Button>
              </li>
            ))}
          </ul>
          {s.published.url && <p className="kst-p kst-dim">{shortLink(s.published.url)}{s.published.code ? " · " + s.published.code : ""}</p>}
        </div>
      )}
      {server && <ServerPack studio={st} version={server} onClose={() => setServer(null)} />}
    </section>
  );
}

/** "Put on a server": a server pack from a version. */
function ServerPack({ studio: st, version, onClose }: { studio: Studio; version: string; onClose: () => void }) {
  const t = useT();
  const servers = useQuery({ queryKey: ["server_list"], queryFn: api.serverList, retry: false });
  const local = (servers.data ?? []).filter((s) => s.kind !== "remote");
  const [pick, setPick] = useState("");
  const [busy, setBusy] = useState(false);
  const go = async () => {
    setBusy(true);
    try {
      const r = await runTask<PackServerInstallResult>(() => api.studioServerPack(st.source.id, version, pick || local[0].id), { failed: t("studio.serverPack.failed") });
      toast.ok(t("studio.serverPack.done", { version }), r.modpack?.file ?? undefined);
      onClose();
    } catch { /* toasted */ } finally { setBusy(false); }
  };
  return (
    <Modal open onClose={onClose} title={t("studio.serverPack.title", { name: st.source.name, version })}
      footer={<><Button variant="ghost" onClick={onClose}>{t("common.cancel")}</Button>
        <Button variant="primary" loading={busy} disabled={!local.length} onClick={() => void go()} data-testid="studio-server-pack-go">{t("studio.serverPack.go")}</Button></>}>
      <p className="kst-p">{t("studio.serverPack.lead")}</p>
      {local.length === 0 ? <p className="kst-p kst-dim">{t("studio.server.none")}</p>
        : <Select value={pick || local[0].id} onChange={setPick} aria-label={t("studio.server.pick")} options={local.map((s) => ({ value: s.id, label: s.name }))} data-testid="studio-server-pick" />}
    </Modal>
  );
}
