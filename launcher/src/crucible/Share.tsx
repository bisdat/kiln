// Share to Crucible on Play (DESIGN 1.2), with "What is Crucible?" beside it (the wiki page in the browser). A profile's
// first share asks first (dev.30.2, players did not expect one click to upload ~20 MB): one screen says what goes up (the
// mod list and the mods' files Crucible doesn't have yet, about how much), what stays (settings, saves, personal files),
// and that the pack is link-only; Share turns the loaded profile into an unlisted pack and copies its link (signed out:
// the Discord sign-in first, then the share goes on by itself), and the popover shows the upload, then "Link copied.
// Paste it to your friends." with the link and the code. Save as file is the way without an account. A profile that
// changed since it was shared carries a "Pack changed" pill, and the popover offers Update link (same link, a new
// version, no confirm). "Got a code?" opens a pack by its code (the clipboard's code is filled in when it opens, and by
// its Paste button; the backend reads the clipboard: ./prefill.ts).
import { useQueryClient, type QueryClient } from "@tanstack/react-query";
import { useEffect, useRef, useState, type ReactNode } from "react";
import * as api from "../api/commands";
import type { BuiltPack, CrucibleShared, GameInstall, ProfileId } from "../api/commands";
import { useGame, useGameSel, useLibrary, useProfile, useProfiles } from "../api/hooks";
import { t, useT } from "../i18n";
import { Button, ProgressBar, Spinner, TextInput, fmtBytes, pickPaths, runTask, toast, useTasks, waitTask } from "../ui";
import { create } from "zustand";
import MARK from "../assets/crucible/mark-64.png";
import { useCommunityUi } from "../community/state";
import { anyChange, codeInput, countsOf, crucibleTarget, shareEstimate } from "./code";
import { clipboardPrefill } from "./prefill";
import { openCrucible } from "./Preview";
import { copyText, crk, crucibleError, readClipboard, shortLink, useCrucibleStatus, useCrucibleUi, useShareRun, useShared } from "./state";

/** The wiki page "What is Crucible?" opens (in the browser). */
export const CRUCIBLE_WIKI = "https://frostdev.io/kiln/wiki/crucible-share";
export const openCrucibleWiki = () => void api.appOpenUrl(CRUCIBLE_WIKI).catch((e) => toast.error(e));

/** Starts the first share after its confirm: signed in, at once; signed out, after the Discord sign-in (Host.tsx's
 *  useShareAfterSignIn starts it then). */
export function confirmShare(qc: QueryClient, profileId: ProfileId, signedIn: boolean, openSignIn: () => void) {
  if (signedIn) { void startShare(qc, profileId); return; }
  useShareRun.getState().set({ profileId, taskId: null, result: null, error: null, copied: null, seen: false, pendingSignIn: true });
  openSignIn();
}

/** What a profile's first share sends at most (code.ts shareEstimate over the profile and the Library). */
export function useShareEstimate(profileId: ProfileId) {
  const profile = useProfile(profileId);
  const library = useLibrary();
  return shareEstimate(profile.data, library.data);
}

/** Which popover of the dock is open (one at a time). */
export const useDock = create<{ open: "share" | "code" | null; set: (o: "share" | "code" | null) => void }>((set) => ({ open: null, set: (open) => set({ open }) }));

/** Starts (or restarts) the share of a profile; the result is copied and remembered in useShareRun. */
export async function startShare(qc: QueryClient, profileId: ProfileId) {
  const runs = useShareRun.getState();
  if (runs.runs[profileId]?.taskId && !runs.runs[profileId]?.result && !runs.runs[profileId]?.error) return;
  runs.set({ profileId, taskId: null, result: null, error: null, copied: null, seen: false, pendingSignIn: false });
  try {
    const id = await api.crucibleShare({ profile: profileId }, useGameSel.getState().gameId);
    useShareRun.getState().set({ ...useShareRun.getState().runs[profileId], taskId: id });
    const r = await waitTask<api.CrucibleShareResult>(id);
    const copied = await copyText(r.pack.url);
    const seen = useDock.getState().open === "share";
    useShareRun.getState().set({ ...useShareRun.getState().runs[profileId], result: r, copied, seen });
    void qc.invalidateQueries({ queryKey: ["crucible_shared"] });
    // closed meanwhile: the toast says it (the task tray showed the upload)
    if (!seen) toast.ok(copied ? t("crucible.share.copied") : t("crucible.share.ready"), shortLink(r.pack.url) + " · " + r.pack.code,
      { actions: [{ label: t("crucible.share.copyLink"), onClick: () => void copyText(r.pack.url).then((ok) => ok && toast.ok(t("crucible.share.linkCopied"))) }], ms: 15_000 });
  } catch (e) {
    const err = api.toKilnError(e);
    useShareRun.getState().set({ ...useShareRun.getState().runs[profileId], error: err, pendingSignIn: err.code === "auth_failed" ? useShareRun.getState().runs[profileId]?.pendingSignIn ?? false : false });
    if (err.code !== "auth_failed" && useDock.getState().open !== "share") toast.error(crucibleError(err), t("crucible.share.failed"));
  }
}

/** A folder the player picks (the browser mock answers itself). */
async function pickFolder(): Promise<string | null> {
  if (!api.isTauri()) return (await api.loadMock()).mockPickFolder();
  const r = await pickPaths({ directory: true, title: t("crucible.save.pick"), kind: "packSave" });
  return r?.[0] ?? null;
}

/** Save as file: the profile as a .kilnpack in a folder the player picks (pack_build). */
export async function saveAsFile(profileId: ProfileId, name: string, version = "1.0.0"): Promise<BuiltPack | null> {
  const dir = await pickFolder();
  if (!dir) return null;
  try {
    const r = await runTask<BuiltPack>(() => api.packBuild({ profile: profileId, version, name }, dir), { failed: t("crucible.save.failed", { name }) });
    toast.ok(t("crucible.save.done", { name }), r.file, { actions: [{ label: t("crucible.save.openFolder"), onClick: () => void api.appOpenPath(dir).catch((e) => toast.error(e)) }] });
    return r;
  } catch { return null; }
}

/** "+1 added, 1 updated": what changed since the share. */
export function changeWords(c: CrucibleShared["changed"]): string {
  const n = countsOf(c);
  return [n.added ? t("crucible.change.added", { count: n.added }) : "", n.updated ? t("crucible.change.updated", { count: n.updated }) : "",
    n.removed ? t("crucible.change.removed", { count: n.removed }) : ""].filter(Boolean).join(", ");
}

/** The share of the loaded profile: Crucible's answer, kept fresh while the profile changes. */
export function useProfileShare(profileId: ProfileId | null) {
  const profiles = useProfiles();
  const stamp = profiles.data?.find((p) => p.id === profileId)?.updated;
  const on = useCrucibleStatus().data?.enabled === true;
  return useShared(profileId, stamp, on);
}

// ---------------------------------------------------------------- the dock under Play

export function ShareDock({ game, profileId, profileName }: { game: GameInstall; profileId: ProfileId; profileName: string }) {
  const t = useT();
  const open = useDock((s) => s.open);
  const setOpen = useDock((s) => s.set);
  const shared = useProfileShare(profileId);
  const changed = !!shared.data?.changed && anyChange(countsOf(shared.data.changed));
  const run = useShareRun((s) => s.runs[profileId]);
  const uploading = !!run?.taskId && !run.result && !run.error;
  const dock = useRef<HTMLDivElement>(null);
  // a click outside the dock and its popover, or Esc, closes the popover (an upload goes on)
  useEffect(() => {
    if (!open) return;
    const down = (e: MouseEvent) => {
      const el = e.target as Element | null;
      if (dock.current?.contains(el) || el?.closest?.('[role="dialog"][aria-modal="true"], .k-toasts, .k-toast')) return;
      setOpen(null);
    };
    const key = (e: KeyboardEvent) => { if (e.key === "Escape" && !document.querySelector('[role="dialog"][aria-modal="true"]')) setOpen(null); };
    window.addEventListener("mousedown", down);
    window.addEventListener("keydown", key);
    return () => { window.removeEventListener("mousedown", down); window.removeEventListener("keydown", key); };
  }, [open, setOpen]);
  useEffect(() => () => setOpen(null), [setOpen]);
  return (
    <div className="kcr-dock" ref={dock}>
      <div className="kcr-dock-row">
        <button type="button" className={"kcr-share-btn" + (open === "share" ? " on" : "")} aria-expanded={open === "share"} data-testid="crucible-share"
          onClick={() => setOpen(open === "share" ? null : "share")}>
          <img src={MARK} alt="" aria-hidden width={22} height={22} />
          <span>{uploading ? t("crucible.share.uploadingShort") : t("crucible.share.button")}</span>
          {changed && !uploading && <span className="kcr-pill" data-testid="crucible-pack-changed">{t("crucible.share.changedPill")}</span>}
        </button>
        <button type="button" className="k-link kcr-code-link" aria-expanded={open === "code"} data-testid="crucible-got-code" onClick={() => setOpen(open === "code" ? null : "code")}>
          {t("crucible.code.link")}
        </button>
        <button type="button" className="k-link kcr-what-link" data-testid="crucible-what" onClick={openCrucibleWiki}>{t("crucible.share.what")}</button>
      </div>
      {open === "share" && <SharePanel game={game} profileId={profileId} profileName={profileName} />}
      {open === "code" && <CodePanel onDone={() => setOpen(null)} />}
    </div>
  );
}

function Panel({ title, children, testid }: { title: ReactNode; children: ReactNode; testid: string }) {
  return (
    <div className="kcr-pop" role="dialog" aria-label={typeof title === "string" ? title : undefined} data-testid={testid}>
      <div className="kcr-pop-title">{title}</div>
      {children}
    </div>
  );
}

function SharePanel({ profileId, profileName }: { game: GameInstall; profileId: ProfileId; profileName: string }) {
  const t = useT();
  const qc = useQueryClient();
  const status = useCrucibleStatus();
  const shared = useProfileShare(profileId);
  const run = useShareRun((s) => s.runs[profileId]);
  const task = useTasks((s) => (run?.taskId ? s.tasks[run.taskId] : undefined));
  const signedIn = !!status.data?.signedIn;
  const uploading = !!run && !run.result && !run.error;
  const [saving, setSaving] = useState(false);
  const s = shared.data;
  const changed = s?.changed && anyChange(countsOf(s.changed)) ? s.changed : null;
  // the result has been seen here
  useEffect(() => { if (run?.result && !run.seen) useShareRun.getState().set({ ...run, seen: true }); }, [run]);
  const save = async () => { setSaving(true); await saveAsFile(profileId, profileName, s?.version.version); setSaving(false); };
  const saveButton = <Button size="s" variant="ghost" loading={saving} onClick={() => void save()} data-testid="crucible-save-file">{t("crucible.save.button")}</Button>;

  if (uploading) {
    const pct = task?.progress != null ? Math.round(task.progress * 100) : null;
    return (
      <Panel title={t("crucible.share.uploadingTitle", { name: profileName })} testid="crucible-share-pop">
        <p className="kcr-p" role="status" data-testid="crucible-share-progress">{task?.message ?? t("crucible.share.starting")}{pct != null && " · " + pct + "%"}</p>
        <ProgressBar value={task?.progress ?? 0} />
        <p className="k-hint">{t("crucible.share.keepsGoing")}</p>
      </Panel>
    );
  }
  if (run?.error && run.error.code !== "auth_failed") {
    return (
      <Panel title={t("crucible.share.failed")} testid="crucible-share-pop">
        <p className="kcr-p k-tone-err" data-testid="crucible-share-error">{crucibleError(run.error)}</p>
        <div className="kcr-pop-actions">
          <Button size="s" variant="primary" onClick={() => void startShare(qc, profileId)}>{t("common.retry")}</Button>
          {saveButton}
        </div>
      </Panel>
    );
  }
  const needSignIn = !signedIn && (!s || !!changed || run?.error?.code === "auth_failed");
  if (status.isLoading || shared.isLoading) return <Panel title={t("crucible.share.button")} testid="crucible-share-pop"><Spinner /></Panel>;
  // never shared and nothing started: say what goes up first (the confirm), then Share or Cancel
  if (!s && !run && !shared.error) {
    return (
      <Panel title={t("crucible.share.confirmTitle", { name: profileName })} testid="crucible-share-pop">
        <ShareConfirm profileId={profileId} signedIn={signedIn} />
        <div className="kcr-pop-actions">
          <Button size="s" variant="primary" data-testid="crucible-share-confirm"
            onClick={() => confirmShare(qc, profileId, signedIn, () => useCommunityUi.getState().openSignIn(false))}>
            {signedIn ? t("crucible.share.confirmShare") : t("crucible.share.confirmSignIn")}
          </Button>
          <Button size="s" variant="ghost" onClick={() => useDock.getState().set(null)} data-testid="crucible-share-cancel">{t("common.cancel")}</Button>
          {!signedIn && <Button size="s" variant="ghost" loading={saving} onClick={() => void save()} data-testid="crucible-save-file">{t("crucible.share.saveInstead")}</Button>}
        </div>
      </Panel>
    );
  }
  if (needSignIn && !s) {
    return (
      <Panel title={t("crucible.share.button")} testid="crucible-share-pop">
        <p className="kcr-p" data-testid="crucible-share-signin">{t("crucible.share.signInText")}</p>
        <div className="kcr-pop-actions">
          <Button size="s" variant="primary" onClick={() => { useShareRun.getState().set({ profileId, taskId: null, result: null, error: null, copied: null, seen: false, pendingSignIn: true }); useCommunityUi.getState().openSignIn(false); }}
            data-testid="crucible-signin">{t("crucible.share.signIn")}</Button>
          <Button size="s" variant="ghost" loading={saving} onClick={() => void save()} data-testid="crucible-save-file">{t("crucible.share.saveInstead")}</Button>
        </div>
      </Panel>
    );
  }
  const result = run?.result ?? null;
  const pack = result?.pack ?? s?.pack ?? null;
  if (!pack) return <Panel title={t("crucible.share.button")} testid="crucible-share-pop"><Spinner /></Panel>;
  const just = !!result;
  return (
    <Panel title={changed ? t("crucible.share.changedTitle") : just ? (run?.copied ? t("crucible.share.copied") : t("crucible.share.ready")) : t("crucible.share.sharedTitle", { name: profileName })} testid="crucible-share-pop">
      {changed ? (
        <div className="kcr-changed" data-testid="crucible-share-changed">
          <p className="kcr-p">{t("crucible.share.updateAsk", { changes: changeWords(changed) })}</p>
          {needSignIn ? (
            <div className="kcr-pop-actions">
              <p className="k-hint">{t("crucible.share.signInText")}</p>
              <Button size="s" variant="primary" onClick={() => { useShareRun.getState().set({ profileId, taskId: null, result: null, error: null, copied: null, seen: false, pendingSignIn: true }); useCommunityUi.getState().openSignIn(false); }}>{t("crucible.share.signIn")}</Button>
            </div>
          ) : <Button size="s" variant="primary" onClick={() => void startShare(qc, profileId)} data-testid="crucible-update-link">{t("crucible.share.updateLink")}</Button>}
        </div>
      ) : just ? <p className="kcr-p" data-testid="crucible-share-done">{run?.copied ? t("crucible.share.pasteIt") : t("crucible.share.copyIt")}</p> : null}
      <LinkRows url={pack.url} code={pack.code} />
      <div className="kcr-pop-actions">{saveButton}{just && result && <span className="k-hint">{t("crucible.share.versionLine", { number: result.version.number })}</span>}</div>
    </Panel>
  );
}

/** The first share's confirm (dev.30.2): what Crucible is, what goes up and about how much, what never does, and that
 *  the pack is link-only (public packs are made in Pack Studio: they need credits, a summary and finished checks).
 *  Shared by the desktop popover and the Deck's dialog. */
export function ShareConfirm({ profileId, signedIn, deck = false }: { profileId: ProfileId; signedIn: boolean; deck?: boolean }) {
  const t = useT();
  const est = useShareEstimate(profileId);
  const line = deck ? "deck-line" : "kcr-p";
  return (
    <div className="kcr-confirm" data-testid="crucible-share-confirm-text">
      <p className={line}>{t("crucible.share.confirmWhat")}</p>
      <ul className="kcr-confirm-list">
        <li data-testid="crucible-share-goes">{est
          ? (est.bytes > 0 ? t("crucible.share.confirmGoesSize", { count: est.mods, size: fmtBytes(est.bytes) }) : t("crucible.share.confirmGoes", { count: est.mods }))
          : t("crucible.share.confirmGoesAny")}</li>
        <li>{t("crucible.share.confirmStays")}</li>
        <li data-testid="crucible-share-never">{t("crucible.share.confirmNever")}</li>
      </ul>
      <div className="kcr-vis" role="radiogroup" aria-label={t("crucible.share.visibility")} data-testid="crucible-share-visibility">
        <label className="kcr-tick"><input type="radio" name={"kcr-vis-" + profileId} checked readOnly className="k-check" data-testid="crucible-share-link-only" />
          <span><b>{t("crucible.share.linkOnly")}</b> {t("crucible.share.linkOnlyText")}</span></label>
        <label className="kcr-tick k-muted"><input type="radio" name={"kcr-vis-" + profileId} checked={false} disabled readOnly className="k-check" data-testid="crucible-share-public" />
          <span><b>{t("crucible.share.public")}</b> {t("crucible.share.publicText")}</span></label>
      </div>
      {!signedIn && <p className={deck ? "deck-note" : "k-hint"} data-testid="crucible-share-signin-first">{t("crucible.share.signInText")}</p>}
    </div>
  );
}

/** The link and the code, each with Copy. */
export function LinkRows({ url, code }: { url: string; code: string }) {
  const t = useT();
  const copy = async (text: string, what: string) => { if (await copyText(text)) toast.ok(what); else toast.info(t("crucible.share.noClipboard")); };
  return (
    <div className="kcr-links">
      <div className="kcr-link-row"><span className="k-mono kcr-ellipsis" data-testid="crucible-link" title={url}>{shortLink(url)}</span>
        <Button size="s" onClick={() => void copy(url, t("crucible.share.linkCopied"))} data-testid="crucible-copy-link">{t("common.copy")}</Button></div>
      <div className="kcr-link-row"><span>{t("crucible.share.codeLabel")} <b className="k-mono kcr-code" data-testid="crucible-code">{code}</b></span>
        <Button size="s" onClick={() => void copy(code, t("crucible.share.codeCopied"))} data-testid="crucible-copy-code">{t("common.copy")}</Button></div>
    </div>
  );
}

// ---------------------------------------------------------------- Got a code?

function CodePanel({ onDone }: { onDone: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const openPreview = useCrucibleUi((s) => s.openPreview);
  const { game } = useGame();
  const [value, setValue] = useState("");
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const typed = useRef(false);
  // the clipboard's code (or link) is filled in when it holds one; Paste does it again (copied after the box opened)
  useEffect(() => {
    let live = true;
    void clipboardPrefill(readClipboard).then((v) => { if (live && v && !typed.current) setValue(v); });
    return () => { live = false; };
  }, []);
  const paste = async () => {
    const v = await clipboardPrefill(readClipboard);
    if (!v) { toast.info(t("crucible.code.pasteNone")); return; }
    typed.current = true;
    setValue(v);
    setErr(null);
  };
  const open = async () => {
    const target = crucibleTarget(value);
    if (!target) { setErr(t("crucible.err.notFound")); return; }
    setBusy(true);
    setErr(null);
    try {
      await qc.fetchQuery({ queryKey: crk.open(target), queryFn: () => openCrucible(target, game?.id), staleTime: 15_000 });
      openPreview({ kind: "crucible", target });
      onDone();
    } catch (e) { setErr(crucibleError(e)); }
    finally { setBusy(false); }
  };
  return (
    <Panel title={t("crucible.code.title")} testid="crucible-code-pop">
      <form className="kcr-code-form" onSubmit={(e) => { e.preventDefault(); void open(); }}>
        <TextInput mono autoFocus value={value} placeholder="H7QK-3MZP" aria-label={t("crucible.code.title")} data-testid="crucible-code-input"
          onChange={(e) => { typed.current = true; const v = e.target.value; setValue(/[:/]/.test(v) ? v.trim() : codeInput(v)); setErr(null); }} />
        <Button type="button" size="s" onClick={() => void paste()} data-testid="crucible-code-paste">{t("crucible.code.paste")}</Button>
        <Button type="submit" variant="primary" size="s" loading={busy} disabled={!value.trim()} data-testid="crucible-code-open">{t("common.open")}</Button>
      </form>
      {err ? <p className="k-hint k-tone-err" role="alert" data-testid="crucible-code-error">{err}</p> : <p className="k-hint">{t("crucible.code.hint")}</p>}
    </Panel>
  );
}
