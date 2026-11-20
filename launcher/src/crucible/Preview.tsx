// The pack preview (DESIGN 1.3): what a pack is, who made it, what it downloads, whether it fits this game and Kiln,
// its optional mods, the mods and their credits, the warning that always shows, and the trust box (unticked by default:
// the owner's security trade-off rule) with "Always trust packs from <name>". Install and play is one task
// (crucible_install, or pack_install for a dropped .kilnpack); the toast after it has Undo (back to the profile before).
// The Deck draws the same model with its own pieces (deck/DeckCrucible.tsx).
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { ShieldAlert } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";
import * as api from "../api/commands";
import type { CruciblePreview, GameInstall, LibraryRef, PackIndex, PackPreview } from "../api/commands";
import { qk, useGame, useGameStatus } from "../api/hooks";
import { KIND_NAME } from "../api/kinds";
import { t, useT } from "../i18n";
import { Banner, Button, Modal, Skeleton, Toggle, fmtBytes, runTask, toast } from "../ui";
import { coverFor } from "./code";
import { usePlayControl } from "../shell/play";
import { crk, crucibleError, refreshCrucible, useCrucibleUi, useUpdates, type PreviewSource } from "./state";

/** One mod of a pack as the preview lists it. */
export interface PreviewMod {
  id: string; name: string; version: string; kind: api.ModKind; author: string | null; source: string | null;
  optional: boolean; fresh: boolean; code: boolean; size: number | null;
}
/** A pack ready to show, from Crucible or from a file. */
export interface PreviewModel {
  source: PreviewSource;
  id: string; title: string; summary: string; creator: { id: string; name: string } | null; version: string; number: number | null;
  upvotes: number | null; comments: number | null; cover: string; code: string | null; link: string | null;
  mods: PreviewMod[]; size: number; download: number; fresh: number; present: number;
  optional: Record<string, boolean>; fits: boolean; builds: number[]; current: number | null;
  kiln: { needs: string | null; ok: boolean }; trusted: boolean; uploading: number | null; instance: string | null;
  /** crucible_install's target (the version id); null for a file. */
  target: string | null; changes: string | null;
}

const has = (list: LibraryRef[], m: { kind: api.ModKind; id: string; version: string }) => list.some((r) => r.kind === m.kind && r.id === m.id && r.version === m.version);

function modsOf(index: PackIndex, missing: LibraryRef[], code: string[]): PreviewMod[] {
  return index.mods.map((m) => ({
    id: m.id, name: m.name ?? m.id, version: m.version, kind: m.kind, author: m.credit?.author ?? null, source: m.credit?.source ?? null,
    optional: m.dep === "optional", fresh: has(missing, m), code: code.includes(m.id), size: m.file?.size ?? null,
  }));
}

export function fromCrucible(p: CruciblePreview, target: string): PreviewModel {
  return {
    source: { kind: "crucible", target }, id: p.pack.id, title: p.pack.title || p.index.name, summary: p.pack.summary, creator: p.pack.owner ? { id: p.pack.owner.id, name: p.pack.owner.name } : null,
    version: p.version.version, number: p.version.number, upvotes: p.pack.upvotes, comments: p.pack.comments, cover: coverFor(p.pack.id, p.pack.icon), code: p.pack.code || null,
    link: p.pack.url || null, mods: modsOf(p.index, p.missing, p.code), size: p.size, download: p.download, fresh: p.missing.filter((r) => !r.id.startsWith("pack.")).length,
    present: p.present.length, optional: p.optional, fits: p.game.fits, builds: p.game.builds, current: p.game.current, kiln: p.kiln, trusted: p.trusted,
    uploading: null, instance: p.instance, target: p.version.id, changes: p.version.changes,
  };
}

/** crucible_open's `busy` answer: the creator's upload runs; only the pack is known yet. */
export function fromUploading(u: api.CrucibleUploading, target: string): PreviewModel {
  const p = u.pack;
  return {
    source: { kind: "crucible", target }, id: p.id, title: p.title, summary: p.summary, creator: p.owner ? { id: p.owner.id, name: p.owner.name } : null,
    version: "", number: null, upvotes: p.upvotes, comments: p.comments, cover: coverFor(p.id, p.icon), code: p.code || null, link: p.url || null,
    mods: [], size: 0, download: 0, fresh: 0, present: 0, optional: {}, fits: true, builds: p.builds, current: null, kiln: { needs: null, ok: true },
    trusted: false, uploading: Math.round(u.uploading.progress * 100), instance: null, target: null, changes: null,
  };
}

/** crucible_open, with the still-uploading answer as a model of its own. */
export async function openCrucible(target: string, gameId?: string | null): Promise<PreviewModel> {
  try { return fromCrucible(await api.crucibleOpen(target, gameId), target); } catch (e) {
    const k = api.toKilnError(e);
    const d = k.detail as api.CrucibleUploading | undefined;
    if (k.code === "busy" && d?.uploading && d.pack) return fromUploading(d, target);
    throw e;
  }
}

export function fromFile(p: PackPreview, path: string, game: GameInstall | null): PreviewModel {
  const builds = p.index.game?.builds ?? [];
  const current = game?.buildId ? Number(game.buildId) : null;
  const sizeOf = (r: LibraryRef) => p.index.mods.find((m) => m.id === r.id)?.file?.size ?? 0;
  return {
    source: { kind: "file", path }, id: p.index.id, title: p.index.name, summary: p.index.description ?? "", creator: p.index.authors?.[0] ? { id: "", name: p.index.authors[0] } : null,
    version: p.index.version, number: null, upvotes: null, comments: null, cover: coverFor(p.index.id), code: null, link: null,
    mods: modsOf(p.index, p.missing, p.code), size: p.size, download: p.missing.reduce((s, r) => s + sizeOf(r), 0), fresh: p.missing.filter((r) => !r.id.startsWith("pack.")).length,
    present: p.present.length, optional: p.optional, fits: !builds.length || current === null || builds.includes(current), builds, current,
    kiln: { needs: null, ok: true }, trusted: false, uploading: null, instance: p.instance ?? null, target: null, changes: p.index.changes ?? null,
  };
}

/** How often Wait asks Crucible again (the lead: about 10 s; the browser mock: 1.5 s). */
export const WAIT_MS = api.isTauri() ? 10_000 : 1500;

/** Loads a preview (asked again every WAIT_MS while the creator's upload runs and the player waits). */
export function usePreview(source: PreviewSource | null, wait = false) {
  const { game } = useGame();
  const q = useQuery({
    queryKey: source?.kind === "file" ? ["pack_inspect", source.path] : crk.open(source?.target ?? ""),
    queryFn: async (): Promise<PreviewModel> => source!.kind === "file"
      ? fromFile(await api.packInspect({ path: source!.path }), source!.path, game)
      : await openCrucible(source!.target, game?.id),
    enabled: !!source, retry: false, staleTime: 15_000,
    refetchInterval: (query) => (wait && query.state.data?.uploading != null ? WAIT_MS : false),
  });
  return q;
}

/** Why Install can't go now (null: it can): no game, Kiln missing, the game running, Kiln too old, still uploading. */
export function installBlock(m: PreviewModel | undefined, game: GameInstall | null, status: api.GameStatus | undefined): string | null {
  if (!m) return null;
  if (!m.kiln.ok) return t("crucible.preview.kilnOld", { version: m.kiln.needs ?? "" });
  if (m.uploading != null) return t("crucible.preview.stillUploading", { name: m.creator?.name ?? m.title, pct: m.uploading });
  if (!game) return t("crucible.preview.noGame");
  if (status && !status.runtime.installed) return t("crucible.preview.installKiln");
  if (status?.running) return t("crucible.err.gameRunning");
  return null;
}

/** "In the Library" / "Download" words for the model's size line. */
export function sizeLine(m: PreviewModel): string {
  if (!m.fresh && m.download < 1_048_576) return t("crucible.preview.allInLibrary", { count: m.mods.length });
  return m.present
    ? t("crucible.preview.downloadSome", { size: fmtBytes(m.download), count: m.present })
    : t("crucible.preview.download", { size: fmtBytes(m.download) });
}

/** Runs the install and answers with the toast (Undo: the profile before). Shared by desktop and Deck. */
export function useInstallPack() {
  const qc = useQueryClient();
  const { game } = useGame();
  return async (m: PreviewModel, choice: { optional: Record<string, boolean>; trustAlways: boolean }, deck = false) => {
    if (!game) throw { code: "not_found", message: t("crucible.preview.noGame") } as api.KilnError;
    const before = qc.getQueryData<api.Activation | null>(qk.active(game.id))?.profileId ?? null;
    let profile: api.Profile, previous: string | null;
    if (m.source.kind === "crucible") {
      const r = await runTask<api.CrucibleInstallResult>(() => api.crucibleInstall(game.id, { target: m.target!, consent: true, trustAlways: choice.trustAlways || undefined,
        optional: choice.optional, play: true }), { quiet: true });
      profile = r.profile; previous = r.previous;
    } else {
      const r = await runTask<api.PackInstallResult>(() => api.packInstall(game.id, { source: { path: (m.source as { path: string }).path }, consent: true, optional: choice.optional, activate: true }), { quiet: true });
      profile = r.profile; previous = before;
    }
    refreshCrucible(qc, game.id);
    if (choice.trustAlways) void qc.invalidateQueries({ queryKey: crk.trust });
    const undo = async () => {
      try {
        if (previous) await api.profileActivate(previous, game.id); else await api.profileDeactivate(game.id);
        refreshCrucible(qc, game.id);
        const name = previous ? qc.getQueryData<api.ProfileSummary[]>(qk.profiles)?.find((p) => p.id === previous)?.name ?? previous : null;
        toast.ok(name ? t("crucible.install.backTo", { name }) : t("crucible.install.backToNone"), t("crucible.install.backDetail", { name: profile.name }));
      } catch (e) { toast.error(e, t("crucible.install.undoFailed")); }
    };
    toast.ok(t("crucible.install.done", { name: profile.name }), t("crucible.install.doneDetail"), deck ? {} : { actions: [{ label: t("common.undo"), onClick: () => void undo() }], ms: 12_000 });
    return { profile, previous, undo };
  };
}

// ---------------------------------------------------------------- desktop

export function PreviewModal({ source }: { source: PreviewSource }) {
  const t = useT();
  const nav = useNavigate();
  const qc = useQueryClient();
  const close = useCrucibleUi((s) => s.closePreview);
  const openUpdate = useCrucibleUi((s) => s.openUpdate);
  const { game } = useGame();
  const status = useGameStatus(game?.id);
  const [wait, setWait] = useState(false);
  const q = usePreview(source, wait);
  const m = q.data;
  const updates = useUpdates(game?.id, !!m?.instance);
  const [trust, setTrust] = useState(false);
  const [always, setAlways] = useState(false);
  const [optional, setOptional] = useState<Record<string, boolean> | null>(null);
  const [showMods, setShowMods] = useState(false);
  const [busy, setBusy] = useState(false);
  const install = useInstallPack();
  const ctl = usePlayControl();
  useEffect(() => { if (m && !optional) setOptional({ ...m.optional }); }, [m, optional]);
  // a pack that finished uploading while the player waited: Install is ready
  useEffect(() => { if (wait && m && m.uploading == null) setWait(false); }, [wait, m]);
  const block = installBlock(m, game, status.data);
  const trusted = !!m?.trusted;
  const ok = !!m && (trust || trusted) && !block && !busy;
  const go = async () => {
    if (!m || !ok) return;
    setBusy(true);
    try {
      const r = await install(m, { optional: optional ?? m.optional, trustAlways: always && !trusted });
      close();
      // "Install and play": the instance is loaded now (the task activated it); the start is the Play control's own
      if (ctl.canPlay) ctl.play("profile", r.profile.id);
    } catch (e) { toast.error(crucibleError(e), t("crucible.install.failed", { name: m.title })); }
    finally { setBusy(false); }
  };
  const update = m?.instance ? updates.data?.find((u) => u.profile === m.instance) ?? null : null;
  const optionalMods = m?.mods.filter((x) => x.optional) ?? [];
  const sizeWord = useMemo(() => (m ? sizeLine(m) : ""), [m]);
  const loadIt = async () => {
    if (!m?.instance || !game) return;
    try {
      await api.profileActivate(m.instance, game.id);
      refreshCrucible(qc, game.id);
      toast.ok(t("crucible.preview.loaded", { name: m.title }));
      close();
    } catch (e) { toast.error(e); }
  };
  const footer = (
    <>
      <Button variant="ghost" onClick={close} disabled={busy}>{t("common.cancel")}</Button>
      {m?.instance ? (update
        ? <Button variant="primary" onClick={() => openUpdate(m.instance!)} data-testid="crucible-preview-see-update">{t("crucible.banner.seeChanges")}</Button>
        : <Button variant="primary" onClick={() => void loadIt()} disabled={!!status.data?.running}>{t("crucible.preview.loadIt")}</Button>)
        : <Button variant="primary" loading={busy} disabled={!ok} onClick={() => void go()} data-testid="crucible-install" title={block ?? (!(trust || trusted) ? t("crucible.preview.tickFirst") : undefined)}>
            {t("crucible.preview.installPlay")}
          </Button>}
    </>
  );
  return (
    <Modal open onClose={close} locked={busy} width={640} title={m ? m.title : t("crucible.preview.title")} footer={footer}>
      <div className="kcr-preview" data-testid="crucible-preview">
        {q.isLoading ? <Skeleton lines={5} /> : q.error ? (
          <div data-testid="crucible-preview-error"><Banner tone="err" title={t("crucible.preview.cantOpen")}>{crucibleError(q.error)}</Banner></div>
        ) : m && (
          <>
            <div className="kcr-cover" style={{ backgroundImage: "url(" + m.cover + ")" }}>
              <div className="kcr-cover-text">
                <span className="kcr-cover-title">{m.title}</span>
                <span className="kcr-cover-sub">
                  {m.creator ? t("crucible.preview.by", { name: m.creator.name }) : t("crucible.preview.fromFile")}
                  {" · "}{m.number != null ? t("crucible.preview.versionN", { version: m.version, number: m.number }) : "v" + m.version}
                  {m.upvotes != null && <> · <span title={t("crucible.preview.upvotes")}>▲ {m.upvotes}</span></>}
                </span>
              </div>
            </div>
            {m.summary && <p className="kcr-p">{m.summary}</p>}
            <ul className="kcr-facts">
              <li data-testid="crucible-fit" className={m.fits ? "" : "k-tone-warn"}>{m.fits
                ? (m.current ? t("crucible.preview.fits", { build: m.current }) : t("crucible.preview.fitsAny"))
                : t("crucible.preview.otherBuild", { build: m.builds[0] ?? "", current: m.current ?? "" })}</li>
              <li data-testid="crucible-size">{sizeWord}</li>
              {m.code && <li className="k-muted">{t("crucible.preview.code", { code: m.code })}</li>}
            </ul>
            {!m.kiln.ok && (
              <div data-testid="crucible-kiln-old"><Banner tone="err" title={t("crucible.preview.kilnOldTitle")} actions={<Button size="s" onClick={() => { close(); nav("/runtime"); }}>{t("crucible.preview.updateKiln")}</Button>}>
                {t("crucible.preview.kilnOld", { version: m.kiln.needs ?? "" })}
              </Banner></div>
            )}
            {m.uploading != null && (
              <div data-testid="crucible-uploading"><Banner tone="info" title={t("crucible.preview.uploadingTitle")} actions={!wait ? <Button size="s" onClick={() => setWait(true)}>{t("crucible.preview.wait")}</Button> : undefined}>
                {wait ? t("crucible.preview.waiting", { name: m.creator?.name ?? m.title, pct: m.uploading }) : t("crucible.preview.stillUploading", { name: m.creator?.name ?? m.title, pct: m.uploading })}
              </Banner></div>
            )}
            {m.instance ? (
              <p className="kcr-p" data-testid="crucible-has-instance">{update ? t("crucible.preview.haveUpdate", { version: update.to.version }) : t("crucible.preview.have")}</p>
            ) : <p className="kcr-p">{t("crucible.preview.ownProfile")}</p>}
            {optionalMods.length > 0 && !m.instance && (
              <div className="kcr-optional" data-testid="crucible-optional">
                <span className="kcr-label">{t("crucible.preview.optional")}</span>
                {optionalMods.map((x) => (
                  <Toggle key={x.id} checked={!!optional?.[x.id]} onChange={(on) => setOptional((o) => ({ ...(o ?? {}), [x.id]: on }))} label={x.name} />
                ))}
              </div>
            )}
            <button type="button" className="k-link" aria-expanded={showMods} onClick={() => setShowMods((s) => !s)} data-testid="crucible-mods-toggle">
              {showMods ? t("crucible.preview.hideMods") : t("crucible.preview.seeMods", { count: m.mods.length })}
            </button>
            {showMods && <ModList mods={m.mods} />}
            {!m.instance && (
              <div className="kcr-trust">
                <div className="kcr-warn" data-testid="crucible-warning"><ShieldAlert size={18} aria-hidden /><span>{t("crucible.preview.warning")}</span></div>
                {trusted ? (
                  <p className="kcr-p k-muted" data-testid="crucible-trusted">{t("crucible.preview.trustedAlready", { name: m.creator?.name ?? "" })}</p>
                ) : (
                  <>
                    <label className="kcr-tick"><input type="checkbox" className="k-check" checked={trust} onChange={(e) => setTrust(e.target.checked)} data-testid="crucible-trust" />
                      <span>{m.creator ? t("crucible.preview.trust", { name: m.creator.name }) : t("crucible.preview.trustFile")}</span></label>
                    {m.creator?.id && (
                      <label className="kcr-tick"><input type="checkbox" className="k-check" checked={always} onChange={(e) => setAlways(e.target.checked)} data-testid="crucible-trust-always" />
                        <span>{t("crucible.preview.trustAlways", { name: m.creator.name })}</span></label>
                    )}
                  </>
                )}
                {block && m.kiln.ok && m.uploading == null && <p className="k-hint" data-testid="crucible-block">{block}</p>}
              </div>
            )}
          </>
        )}
      </div>
    </Modal>
  );
}

/** The pack's mods with their credits: name and version, by whom, where from, new or already here, code. */
export function ModList({ mods }: { mods: PreviewMod[] }) {
  const t = useT();
  return (
    <ul className="kcr-mods" data-testid="crucible-mods">
      {mods.map((x) => (
        <li key={x.kind + x.id}>
          <span className="kcr-mod-main">
            <span className="kcr-mod-name">{x.name} <span className="k-muted">{x.version}</span></span>
            <span className="kcr-mod-sub">
              {x.author ? t("crucible.preview.modBy", { name: x.author }) : KIND_NAME[x.kind]}
              {x.source && <> · <button type="button" className="k-link" onClick={() => api.appOpenUrl(x.source!).catch((e) => toast.error(e))}>{hostOf(x.source)}</button></>}
            </span>
          </span>
          <span className="kcr-mod-tags">
            {x.code && <span className="k-pill k-tone-warn" title={t("crucible.preview.codeTip")}>{t("crucible.preview.codeTag")}</span>}
            {x.optional && <span className="k-pill">{t("crucible.preview.optionalTag")}</span>}
            <span className={"k-pill" + (x.fresh ? " k-tone-ember" : "")}>{x.fresh ? (x.size ? fmtBytes(x.size) : t("crucible.preview.newTag")) : t("crucible.preview.inLibrary")}</span>
          </span>
        </li>
      ))}
    </ul>
  );
}

const hostOf = (url: string) => { try { return new URL(url).host.replace(/^www\./, ""); } catch { return url; } };
