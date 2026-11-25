// The pack preview (DESIGN 1.3): what a pack is, who made it, what it downloads, whether it fits this game and Kiln,
// its optional mods, the mods and their credits, the warning that always shows, and the trust box (unticked by default:
// the owner's security trade-off rule) with "Always trust packs from <name>", the new profile's name (the pack and its
// creator, never a name a profile has), and Crucible's check line. Install and play is one task (crucible_install, or
// pack_install for a dropped .kilnpack); the toast after it has Undo (back to the profile before). Install alone makes the
// profile without loading it or starting the game: only the data folder changes, so it may run while the game does.
// The Deck draws the same model with its own pieces (deck/DeckCrucible.tsx). A pack installed already whose profile lacks
// mods of its installed version in the Library (the player removed one) says so before Load it, and offers Download
// missing mods (crucible_repair: only those mods, into the Library; the profile, its version and settings stay, nothing
// is loaded or started, so it may run while the game does). Load it alone would start the game without them: with direct
// paths the activation names their missing Library folders, and only Copy mode refuses ("not in the library").
import { useQuery, useQueryClient, type QueryClient } from "@tanstack/react-query";
import { ShieldAlert } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";
import * as api from "../api/commands";
import type { CrucibleChecks, CruciblePreview, GameInstall, LibraryRef, PackIndex, PackPreview, ProfileId, GameId } from "../api/commands";
import { qk, useGame, useGameStatus, useProfiles } from "../api/hooks";
import { KIND_NAME } from "../api/kinds";
import { t, useT } from "../i18n";
import { Banner, Button, Modal, Skeleton, TextInput, Toggle, fmtBytes, runTask, toast, useTasks } from "../ui";
import { checksLine, coverFor, uniqueName } from "./code";
import { missingCount, missingLine, repairLabel } from "./repair";
export { missingCount, missingLine, repairLabel } from "./repair";
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
  /** Crucible's check of this version (null: a file, still uploading, or an older Crucible). */
  checks: CrucibleChecks | null;
  /** What the installed instance's profile lacks of its installed version (null: no instance, or nothing known). */
  repair: api.PackLacking | null;
  /** Where Download missing mods takes them from: Crucible, this file (it is the installed version), or nowhere here. */
  repairFrom: "crucible" | "file" | null;
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
    uploading: null, instance: p.instance, target: p.version.id, changes: p.version.changes, checks: p.version.checks ?? null,
    repair: p.instance ? p.repair ?? null : null, repairFrom: p.instance ? "crucible" : null,
  };
}

/** crucible_open's `busy` answer: the creator's upload runs; only the pack is known yet. */
export function fromUploading(u: api.CrucibleUploading, target: string): PreviewModel {
  const p = u.pack;
  return {
    source: { kind: "crucible", target }, id: p.id, title: p.title, summary: p.summary, creator: p.owner ? { id: p.owner.id, name: p.owner.name } : null,
    version: "", number: null, upvotes: p.upvotes, comments: p.comments, cover: coverFor(p.id, p.icon), code: p.code || null, link: p.url || null,
    mods: [], size: 0, download: 0, fresh: 0, present: 0, optional: {}, fits: true, builds: p.builds, current: null, kiln: { needs: null, ok: true },
    trusted: false, uploading: Math.round(u.uploading.progress * 100), instance: null, target: null, changes: null, checks: null,
    repair: null, repairFrom: null,
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
    kiln: { needs: null, ok: true }, trusted: false, uploading: null, instance: p.instance ?? null, target: null, changes: p.index.changes ?? null, checks: null,
    repair: p.instance ? p.repair ?? null : null, repairFrom: p.instance && p.installedVersion ? "file" : null,
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
    // a code opened again reads the Library afresh (a pack mod removed meanwhile is missing now)
    enabled: !!source, retry: false, staleTime: 15_000, refetchOnMount: "always",
    refetchInterval: (query) => (wait && query.state.data?.uploading != null ? WAIT_MS : false),
  });
  return q;
}

/** The Kiln version a pack needs, as a player reads it: "0.2.0-dev.31" for ">=0.2.0-dev.31" (the line says "or newer"). */
export const neededKiln = (m: PreviewModel) => (m.kiln.needs ?? "").replace(/^[\s>=~^v]+/, "");

/** Why Install can't go now (null: it can): no game, Kiln missing, the game running, Kiln too old, still uploading.
 *  `play` false (Install alone): the game running and Kiln missing from it don't matter, since the install writes only
 *  the data folder (the Library and the new profile; kiln_manage::crucible::install with play false). */
export function installBlock(m: PreviewModel | undefined, game: GameInstall | null, status: api.GameStatus | undefined, play = true): string | null {
  if (!m) return null;
  if (!m.kiln.ok) return t("crucible.preview.kilnOld", { version: neededKiln(m) });
  if (m.uploading != null) return t("crucible.preview.stillUploading", { name: m.creator?.name ?? m.title, pct: m.uploading });
  if (!game) return t("crucible.preview.noGame");
  if (!play) return null;
  if (status && !status.runtime.installed) return t("crucible.preview.installKiln");
  if (status?.running) return t("crucible.err.gameRunning");
  return null;
}

/** The new profile's name: the pack and its creator ("My mods (Felup's pack)"), or the pack's alone without one; never
 *  a name a profile has (" (2)"). */
export function defaultProfileName(m: PreviewModel, taken: Iterable<string>): string {
  const base = m.creator?.name?.trim() ? t("crucible.install.profileName", { pack: m.title, creator: m.creator.name.trim() }) : m.title;
  return uniqueName(base, taken);
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
  /** `play` false: Install alone (no activation, no start; the toast has no Undo, the loaded profile stays). `name`: the
   *  name field's (default: defaultProfileName). */
  return async (m: PreviewModel, choice: { optional: Record<string, boolean>; trustAlways: boolean; name?: string; play?: boolean }, deck = false) => {
    if (!game) throw { code: "not_found", message: t("crucible.preview.noGame") } as api.KilnError;
    const play = choice.play !== false;
    const taken = (qc.getQueryData<api.ProfileSummary[]>(qk.profiles) ?? []).map((p) => p.name);
    const name = uniqueName(choice.name?.trim() || defaultProfileName(m, taken), taken);
    const before = qc.getQueryData<api.Activation | null>(qk.active(game.id))?.profileId ?? null;
    let profile: api.Profile, previous: string | null;
    if (m.source.kind === "crucible") {
      const r = await runTask<api.CrucibleInstallResult>(() => api.crucibleInstall(game.id, { target: m.target!, consent: true, trustAlways: choice.trustAlways || undefined,
        optional: choice.optional, name, play }), { quiet: true });
      profile = r.profile; previous = r.previous;
    } else {
      const r = await runTask<api.PackInstallResult>(() => api.packInstall(game.id, { source: { path: (m.source as { path: string }).path }, consent: true, optional: choice.optional, name, activate: play }), { quiet: true });
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
    if (!play) toast.ok(t("crucible.install.done", { name: profile.name }), t("crucible.install.doneOnly"), { ms: 12_000 });
    else toast.ok(t("crucible.install.done", { name: profile.name }), t("crucible.install.doneDetail"), deck ? {} : { actions: [{ label: t("common.undo"), onClick: () => void undo() }], ms: 12_000 });
    return { profile, previous, undo };
  };
}

/** Runs Download missing mods (crucible_repair) and answers with the toast; `onTask`: its task id (progress). Shared by
 *  the preview and Settings > Profiles. Never activates or starts anything. */
export function useRepairPack() {
  const qc = useQueryClient();
  const { game } = useGame();
  return (profileId: ProfileId, name: string, opts: { path?: string; onTask?: (id: string) => void } = {}) => repairPack(qc, game?.id ?? null, profileId, name, opts);
}

/** [useRepairPack] outside a component (Settings > Profiles' menu). */
export async function repairPack(qc: QueryClient, gameId: GameId | null, profileId: ProfileId, name: string, opts: { path?: string; onTask?: (id: string) => void } = {}) {
  try {
    const r = await runTask<api.PackRepairResult>(async () => {
      const id = await api.crucibleRepair(gameId, { profile: profileId, ...(opts.path ? { path: opts.path } : {}) });
      opts.onTask?.(id);
      return id;
    }, { quiet: true });
    refreshCrucible(qc, gameId);
    void qc.invalidateQueries({ queryKey: crk.lacking(profileId) });
    if (r.left.length) toast.info(t("crucible.repair.partly", { name, count: r.imported.length }), t("crucible.repair.left", { count: r.left.length, names: r.left.map((x) => x.id).join(", ") }));
    else if (!r.imported.length) toast.ok(t("crucible.repair.nothing", { name }));
    else toast.ok(t("crucible.repair.done", { name, count: r.imported.length }), r.downloaded ? t("crucible.repair.doneDetail", { size: fmtBytes(r.downloaded) }) : t("crucible.repair.doneLocal"));
    return r;
  } catch (e) {
    toast.error(crucibleError(e), t("crucible.repair.failed", { name }));
    throw e;
  }
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
  const [busy, setBusy] = useState<"install" | "play" | "repair" | null>(null);
  const [repairTask, setRepairTask] = useState<string | null>(null);
  const task = useTasks((s) => (repairTask ? s.tasks[repairTask] : undefined));
  const install = useInstallPack();
  const repairPack = useRepairPack();
  const ctl = usePlayControl();
  const profiles = useProfiles();
  const taken = useMemo(() => (profiles.data ?? []).map((p) => p.name), [profiles.data]);
  // the new profile's name: the pack and its creator, never one a profile has (the player may change it)
  const [name, setName] = useState<string | null>(null);
  useEffect(() => { if (m && name === null && !profiles.isLoading) setName(defaultProfileName(m, taken)); }, [m, name, profiles.isLoading, taken]);
  const nameTaken = !!name?.trim() && taken.some((n) => n.trim().toLowerCase() === name.trim().toLowerCase());
  useEffect(() => { if (m && !optional) setOptional({ ...m.optional }); }, [m, optional]);
  // a pack that finished uploading while the player waited: Install is ready
  useEffect(() => { if (wait && m && m.uploading == null) setWait(false); }, [wait, m]);
  const checks = useQuery({ queryKey: ["pack_check", source.kind === "file" ? source.path : ""], enabled: source.kind === "file",
    queryFn: () => api.packCheck((source as { path: string }).path), retry: false, staleTime: 5 * 60_000 });
  const checkErrors = checks.data?.findings.filter((f) => f.level === "error") ?? [];
  const fileBlock = source.kind !== "file" ? null : checks.isLoading ? t("crucible.checks.running")
    : checks.error ? t("crucible.checks.couldNot") : checkErrors.length || checks.data?.ok === false ? t("crucible.checks.failedBlock") : null;
  // Install and play needs the game closed and Kiln in it; Install alone touches neither (installBlock)
  const block = installBlock(m, game, status.data) ?? fileBlock;
  const blockInstall = installBlock(m, game, status.data, false) ?? fileBlock;
  const trusted = !!m?.trusted;
  const ticked = !!m && (trust || trusted) && !busy;
  const ok = ticked && !block;
  const okInstall = ticked && !blockInstall;
  const go = async (play: boolean) => {
    if (!m || !(play ? ok : okInstall)) return;
    setBusy(play ? "play" : "install");
    try {
      const r = await install(m, { optional: optional ?? m.optional, trustAlways: always && !trusted, name: name ?? undefined, play });
      close();
      // "Install and play": the instance is loaded now (the task activated it); the start is the Play control's own
      if (play && ctl.canPlay) ctl.play("profile", r.profile.id);
    } catch (e) { toast.error(crucibleError(e), t("crucible.install.failed", { name: m.title })); }
    finally { setBusy(null); }
  };
  const hint = blockInstall ?? (block && status.data?.running ? t("crucible.preview.runningHint") : block);
  const update = m?.instance ? updates.data?.find((u) => u.profile === m.instance) ?? null : null;
  const optionalMods = m?.mods.filter((x) => x.optional) ?? [];
  const sizeWord = useMemo(() => (m ? sizeLine(m) : ""), [m]);
  const missing = missingCount(m);
  const repair = async () => {
    if (!m?.instance || !m.repair || !m.repairFrom) return;
    setBusy("repair");
    try {
      await repairPack(m.instance, m.title, { path: m.repairFrom === "file" ? (source as { path: string }).path : undefined, onTask: setRepairTask });
      await q.refetch();
    } catch { /* the toast said it */ } finally { setBusy(null); setRepairTask(null); }
  };
  const repairButton = m?.repair && missing > 0 && m.repairFrom ? (
    <Button variant={update ? undefined : "primary"} loading={busy === "repair"} disabled={!!busy && busy !== "repair"} onClick={() => void repair()} data-testid="crucible-repair">
      {repairLabel(m.repair, m.repairFrom)}
    </Button>
  ) : null;
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
      <Button variant="ghost" onClick={close} disabled={!!busy}>{t("common.cancel")}</Button>
      {/* missing mods: Download missing mods next to See changes or Load it (the update is never taken silently) */}
      {m?.instance ? (update
        ? <>{repairButton}<Button variant="primary" onClick={() => openUpdate(m.instance!)} disabled={busy === "repair"} data-testid="crucible-preview-see-update">{t("crucible.banner.seeChanges")}</Button></>
        : <>
            <Button variant={repairButton ? undefined : "primary"} onClick={() => void loadIt()} disabled={!!status.data?.running || busy === "repair"} data-testid="crucible-load-it">{t("crucible.preview.loadIt")}</Button>
            {repairButton}
          </>)
        : <>
            <Button loading={busy === "install"} disabled={!okInstall} onClick={() => void go(false)} data-testid="crucible-install-only"
              title={blockInstall ?? (!(trust || trusted) ? t("crucible.preview.tickFirst") : t("crucible.preview.installOnlyTip"))}>
              {t("common.install")}
            </Button>
            <Button variant="primary" loading={busy === "play"} disabled={!ok} onClick={() => void go(true)} data-testid="crucible-install" title={block ?? (!(trust || trusted) ? t("crucible.preview.tickFirst") : undefined)}>
              {t("crucible.preview.installPlay")}
            </Button>
          </>}
    </>
  );
  return (
    <Modal open onClose={close} locked={!!busy} width={640} title={m ? m.title : t("crucible.preview.title")} footer={footer}>
      <div className="kcr-preview" data-testid="crucible-preview">
        {q.isLoading ? <Skeleton lines={5} /> : q.error ? (
          <div data-testid="crucible-preview-error"><Banner tone="err" title={t("crucible.preview.cantOpen")}>{crucibleError(q.error)}</Banner></div>
        ) : m && (
          <>
            {source.kind === "crucible" && source.resume && <p className="kcr-p" data-testid="crucible-resumed">{t("crucible.preview.resumed", { name: m.title })}</p>}
            <div className="kcr-cover" style={{ backgroundImage: "url(" + m.cover + ")" }}>
              {/* the dialog's title names the pack: the cover says by whom, which version and its votes */}
              <div className="kcr-cover-text">
                <span className="kcr-cover-sub">
                  {m.creator ? t("crucible.preview.by", { name: m.creator.name }) : t("crucible.preview.fromFile")}
                  {m.version && <>{" · "}{m.number != null ? t("crucible.preview.versionN", { version: m.version, number: m.number }) : "v" + m.version}</>}
                  {m.upvotes != null && <> · <span title={t("crucible.preview.upvotes")}>▲ {m.upvotes}</span></>}
                </span>
              </div>
            </div>
            {m.summary && <p className="kcr-p">{m.summary}</p>}
            <ul className="kcr-facts">
              <li data-testid="crucible-fit" className={m.fits ? "" : "k-tone-warn"}>{m.fits
                ? (m.current ? t("crucible.preview.fits", { build: m.current }) : t("crucible.preview.fitsAny"))
                : t("crucible.preview.otherBuild", { build: m.builds[0] ?? "", current: m.current ?? "" })}</li>
              {m.uploading == null && <li data-testid="crucible-size">{sizeWord}</li>}
              {source.kind === "crucible" && <ChecksFact checks={m.checks} busy={q.isFetching} onRefresh={() => void q.refetch()} />}
              {m.code && <li className="k-muted">{t("crucible.preview.code", { code: m.code })}</li>}
            </ul>
            {!m.kiln.ok && (
              <div data-testid="crucible-kiln-old"><Banner tone="err" title={t("crucible.preview.kilnOldTitle")} actions={<Button size="s" onClick={() => { close(); nav("/runtime"); }}>{t("crucible.preview.updateKiln")}</Button>}>
                {t("crucible.preview.kilnOld", { version: neededKiln(m) })}
              </Banner></div>
            )}
            {m.uploading != null && (
              <div data-testid="crucible-uploading"><Banner tone="info" title={t("crucible.preview.uploadingTitle")} actions={!wait ? <Button size="s" onClick={() => setWait(true)}>{t("crucible.preview.wait")}</Button> : undefined}>
                {wait ? t("crucible.preview.waiting", { name: m.creator?.name ?? m.title, pct: m.uploading }) : t("crucible.preview.stillUploading", { name: m.creator?.name ?? m.title, pct: m.uploading })}
              </Banner></div>
            )}
            {source.kind === "file" && <Checks q={checks} />}
            {m.instance ? (
              <p className="kcr-p" data-testid="crucible-has-instance">{update ? t("crucible.preview.haveUpdate", { version: update.to.version }) : t("crucible.preview.have")}</p>
            ) : <p className="kcr-p">{t("crucible.preview.ownProfile")}</p>}
            {m.instance && m.repair && missing > 0 && (
              <div data-testid="crucible-missing"><Banner tone="warn" title={t("crucible.repair.title", { count: missing })}>
                {missingLine(m.repair)}{!m.repairFrom && <> {t("crucible.repair.needFile", { version: m.repair.version })}</>}
              </Banner></div>
            )}
            {busy === "repair" && task && (
              <p className="k-hint" data-testid="crucible-repair-progress">{task.message ?? t("crucible.repair.running")}{task.progress != null ? " · " + Math.round(task.progress * 100) + "%" : ""}</p>
            )}
            {optionalMods.length > 0 && !m.instance && (
              <div className="kcr-optional" data-testid="crucible-optional">
                <span className="kcr-label">{t("crucible.preview.optional")}</span>
                {optionalMods.map((x) => (
                  <Toggle key={x.id} checked={!!optional?.[x.id]} onChange={(on) => setOptional((o) => ({ ...(o ?? {}), [x.id]: on }))} label={x.name} />
                ))}
              </div>
            )}
            {m.mods.length > 0 && (
              <button type="button" className="k-link" aria-expanded={showMods} onClick={() => setShowMods((s) => !s)} data-testid="crucible-mods-toggle">
                {showMods ? t("crucible.preview.hideMods") : t("crucible.preview.seeMods", { count: m.mods.length })}
              </button>
            )}
            {showMods && <ModList mods={m.mods} />}
            {!m.instance && (
              <div className="kcr-name">
                <label className="kcr-label" htmlFor="kcr-profile-name">{t("crucible.preview.nameLabel")}</label>
                <TextInput id="kcr-profile-name" value={name ?? ""} maxLength={128} disabled={!!busy} data-testid="crucible-profile-name"
                  onChange={(e) => setName(e.target.value)} onBlur={() => { if (m && !name?.trim()) setName(defaultProfileName(m, taken)); }} />
                {nameTaken && <p className="k-hint" data-testid="crucible-name-taken">{t("crucible.preview.nameTaken", { name: uniqueName(name ?? "", taken) })}</p>}
              </div>
            )}
            {!m.instance && (
              <div className="kcr-trust">
                <div className="kcr-warn" data-testid="crucible-warning"><ShieldAlert size={18} aria-hidden /><span>{source.kind === "file" ? t("crucible.preview.warningFile") : t("crucible.preview.warning")}</span></div>
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
                {hint && m.kiln.ok && m.uploading == null && <p className="k-hint" data-testid="crucible-block">{hint}</p>}
                {/* why Install waits, said where the eye is (the disabled buttons' tooltips are out of reach for the keyboard) */}
                {!hint && !trusted && !trust && <p className="k-hint kcr-tick-hint" data-testid="crucible-tick-first">{t("crucible.preview.tickFirst")}</p>}
              </div>
            )}
          </>
        )}
      </div>
    </Modal>
  );
}

/** Crucible's check of the pack as one line of the facts (code.ts checksLine): passed, things to look at, problems
 *  (named, they don't block: Crucible keeps a pack with errors from going public), still running with Refresh (the full
 *  check ends a few minutes after the upload), or could not run. Nothing from an older Crucible without checks. */
function ChecksFact({ checks, busy, onRefresh }: { checks: CrucibleChecks | null; busy: boolean; onRefresh: () => void }) {
  const t = useT();
  const line = checksLine(checks);
  if (!line) return null;
  const text = line.kind === "passed" ? t("crucible.checks.passed") : line.kind === "warnings" ? t("crucible.checks.lineWarnings", { count: line.count })
    : line.kind === "problems" ? t("crucible.checks.lineProblems", { count: line.count }) : line.kind === "pending" ? t("crucible.checks.pending") : t("crucible.checks.lineFailed");
  const tone = line.kind === "problems" ? "k-tone-err" : line.kind === "warnings" ? "k-tone-warn" : line.kind === "passed" ? "" : "k-muted";
  return (
    <li className={"kcr-checks " + tone} data-testid="crucible-checks" data-state={line.kind}>
      <span>{text}</span>
      {line.kind === "pending" && <> <button type="button" className="k-link" disabled={busy} onClick={onRefresh} data-testid="crucible-checks-refresh">{t("common.refresh")}</button></>}
      {line.messages.length > 0 && <ul className="kcr-findings">{line.messages.map((x, i) => <li key={i}>{x}</li>)}</ul>}
    </li>
  );
}

/** The pack analysis of a file (no Crucible result exists for one): errors in red (they block the install), warnings
 *  in yellow, "Checks: passed" when there is nothing to say. */
function Checks({ q }: { q: { isLoading: boolean; error: unknown; data?: api.PackCheckReport } }) {
  const t = useT();
  if (q.isLoading) return <p className="k-hint" data-testid="crucible-checks">{t("crucible.checks.running")}</p>;
  if (q.error || !q.data) return <div data-testid="crucible-checks"><Banner tone="err" title={t("crucible.checks.couldNot")}>{crucibleError(q.error)}</Banner></div>;
  const errors = q.data.findings.filter((f) => f.level === "error");
  const warnings = q.data.findings.filter((f) => f.level === "warning");
  const line = (f: api.PackCheckFinding, i: number) => <li key={i}>{f.mod ? <span className="k-mono">{f.mod}: </span> : null}{f.message}</li>;
  return (
    <div data-testid="crucible-checks">
      {errors.length > 0 && <Banner tone="err" title={t("crucible.checks.failed", { count: errors.length })}><ul className="kcr-findings">{errors.slice(0, 20).map(line)}</ul></Banner>}
      {warnings.length > 0 && <Banner tone="warn" title={t("crucible.checks.warnings", { count: warnings.length })}><ul className="kcr-findings">{warnings.slice(0, 20).map(line)}</ul></Banner>}
      {!errors.length && !warnings.length && <p className="k-hint">{t("crucible.checks.passed")}</p>}
    </div>
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
