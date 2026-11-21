// Crucible on the Deck (DESIGN 2): Home's "Share to Crucible" (a profile's first share asks first, as on the desktop:
// what goes up, then A shares the loaded profile; then the link's QR code and the big code, with the upload's progress)
// and "Enter a code" (8 symbols with the Steam keyboard, Open), and the pack preview with gamepad focus starting on
// Install and play: the same warning, trust boxes, optional mods and check line as on the desktop
// (src/crucible/Preview.tsx's model). kiln://crucible links open the same preview.
import { useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import * as api from "../api/commands";
import type { ProfileId } from "../api/commands";
import { useGame, useGameStatus } from "../api/hooks";
import { useT } from "../i18n";
import { ProgressBar, useTasks } from "../ui";
import { QrCode } from "../community/parts";
import { DeckSignIn } from "../community/SignIn";
import { DeckButton, Dialog, Row, Word } from "../deck/ui";
import { anyChange, checksLine, codeInput, countsOf, crucibleTarget } from "./code";
import { installBlock, neededKiln, openCrucible, sizeLine, useInstallPack, usePreview, type PreviewModel } from "./Preview";
import { ShareConfirm, changeWords, startShare, useProfileShare } from "./Share";
import { useCrucibleLinks } from "./Host";
import { crk, crucibleError, useCrucibleStatus, useCrucibleUi, useShareRun, useStatusFollowsSignIn } from "./state";
import "./crucible.css";

export function DeckShareDialog({ profileId, profileName, onClose }: { profileId: ProfileId; profileName: string; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const status = useCrucibleStatus();
  const shared = useProfileShare(profileId);
  const run = useShareRun((s) => s.runs[profileId]);
  const task = useTasks((s) => (run?.taskId ? s.tasks[run.taskId] : undefined));
  const [signIn, setSignIn] = useState(false);
  const signedIn = !!status.data?.signedIn;
  const s = shared.data;
  const changed = s?.changed && anyChange(countsOf(s.changed)) ? s.changed : null;
  const uploading = !!run && !run.result && !run.error && !run.pendingSignIn;
  // a first share asks first (what goes up); A then shares, or signs in and shares
  const confirm = !!status.data && !shared.isLoading && !s && !run;
  const pack = run?.result?.pack ?? s?.pack ?? null;
  let body;
  if (confirm) {
    body = <div className="deck-list" data-testid="deck-crucible-confirm"><ShareConfirm profileId={profileId} signedIn={signedIn} deck /></div>;
  } else if (uploading) {
    body = (
      <div className="deck-list" data-testid="deck-crucible-uploading">
        <p className="deck-line" role="status">{task?.message ?? t("crucible.share.starting")}{task?.progress != null && " · " + Math.round(task.progress * 100) + "%"}</p>
        <ProgressBar value={task?.progress ?? 0} />
        <p className="deck-note">{t("crucible.share.keepsGoing")}</p>
      </div>
    );
  } else if (!signedIn && !s) {
    body = <p className="deck-line" data-testid="deck-crucible-signin">{t("crucible.share.signInText")}</p>;
  } else if (run?.error && run.error.code !== "auth_failed") {
    body = <p className="deck-line"><Word tone="err">{crucibleError(run.error)}</Word></p>;
  } else if (pack) {
    body = (
      <div className="kcr-deck-share" data-testid="deck-crucible-shared">
        <QrCode text={pack.url} size={232} title={pack.url} />
        <div className="kcr-deck-share-text">
          <div className="kcr-deck-big" data-testid="deck-crucible-code">{pack.code}</div>
          <p className="deck-line">{t("crucible.deck.scan")}</p>
          <p className="deck-note k-mono">{pack.url.replace(/^https:\/\//, "")}</p>
          {changed && <p className="deck-line" data-testid="deck-crucible-changed"><Word tone="warn">{t("crucible.share.changedPill")}</Word> {t("crucible.share.updateAsk", { changes: changeWords(changed) })}</p>}
        </div>
      </div>
    );
  } else body = <p className="deck-line">{t("crucible.share.starting")}</p>;
  const buttons = uploading ? undefined
    : confirm && signedIn ? <DeckButton primary onClick={() => void startShare(qc, profileId)} id="crucible-share-confirm" autoFocus>{t("crucible.share.confirmShare")}</DeckButton>
    : confirm ? <DeckButton primary onClick={() => setSignIn(true)} id="crucible-share-confirm" autoFocus>{t("crucible.share.confirmSignIn")}</DeckButton>
    : !signedIn && (!s || changed) ? <DeckButton primary onClick={() => setSignIn(true)}>{t("crucible.share.signIn")}</DeckButton>
    : changed ? <DeckButton primary onClick={() => void startShare(qc, profileId)}>{t("crucible.share.updateLink")}</DeckButton>
    : run?.error && run.error.code !== "auth_failed" ? <DeckButton primary onClick={() => void startShare(qc, profileId)}>{t("common.retry")}</DeckButton>
    : <DeckButton primary onClick={onClose}>{t("common.ok")}</DeckButton>;
  return (
    <>
      <Dialog name="crucible-share" title={t("crucible.deck.shareTitle", { name: profileName })} wide onClose={onClose} backLabel={t("common.close")} buttons={buttons}>
        {body}
      </Dialog>
      {signIn && <DeckSignIn onClose={() => setSignIn(false)} onDone={() => { setSignIn(false); void qc.invalidateQueries({ queryKey: crk.status }).then(() => startShare(qc, profileId)); }} />}
    </>
  );
}

export function DeckCodeDialog({ onClose }: { onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const { game } = useGame();
  const openPreview = useCrucibleUi((s) => s.openPreview);
  const [value, setValue] = useState("");
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  useEffect(() => { api.communitySteamKeyboard().catch(() => {}); }, []);
  const open = async () => {
    const target = crucibleTarget(value);
    if (!target) { setErr(t("crucible.err.notFound")); return; }
    setBusy(true); setErr(null);
    try {
      await qc.fetchQuery({ queryKey: crk.open(target), queryFn: () => openCrucible(target, game?.id), staleTime: 15_000 });
      onClose();
      openPreview({ kind: "crucible", target });
    } catch (e) { setErr(crucibleError(e)); } finally { setBusy(false); }
  };
  return (
    <Dialog name="crucible-code" title={t("crucible.code.title")} onClose={onClose}
      buttons={<DeckButton primary disabled={busy || !value} onClick={() => void open()}>{t("common.open")}</DeckButton>}>
      <input className="kdc-input kcr-deck-input" data-deck-autofocus="" data-testid="deck-crucible-code-input" aria-label={t("crucible.code.title")} value={value} placeholder="H7QK-3MZP"
        onChange={(e) => { setValue(codeInput(e.target.value)); setErr(null); }} onKeyDown={(e) => { if (e.key === "Enter") void open(); }} />
      {err ? <p className="deck-line" role="alert" data-testid="deck-crucible-code-error"><Word tone="warn">{err}</Word></p> : <p className="deck-note">{t("crucible.deck.codeHint")}</p>}
    </Dialog>
  );
}

export function DeckPreviewDialog({ source, onClose }: { source: NonNullable<ReturnType<typeof useCrucibleUi.getState>["preview"]>; onClose: () => void }) {
  const t = useT();
  const { game } = useGame();
  const status = useGameStatus(game?.id);
  const [wait, setWait] = useState(false);
  const q = usePreview(source, wait);
  const m = q.data;
  const [trust, setTrust] = useState(false);
  const [always, setAlways] = useState(false);
  const [optional, setOptional] = useState<Record<string, boolean> | null>(null);
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const [done, setDone] = useState<{ name: string; undo: () => Promise<void> } | null>(null);
  const install = useInstallPack();
  useEffect(() => { if (m && !optional) setOptional({ ...m.optional }); }, [m, optional]);
  const block = installBlock(m, game, status.data);
  const ready = !!m && (trust || m.trusted) && !block && !m.instance && !busy;
  const go = async () => {
    if (!m) return;
    if (!ready) { setNote(block ?? (m.instance ? t("crucible.preview.have") : t("crucible.preview.tickFirst"))); return; }
    setBusy(true);
    try { const r = await install(m, { optional: optional ?? m.optional, trustAlways: always && !m.trusted }, true); setDone({ name: r.profile.name, undo: r.undo }); }
    catch (e) { setNote(crucibleError(e)); }
    finally { setBusy(false); }
  };
  if (done) {
    return (
      <Dialog name="crucible-done" title={t("crucible.install.done", { name: done.name })} onClose={onClose} backLabel={t("common.close")}
        buttons={<><DeckButton onClick={() => { void done.undo(); onClose(); }} id="crucible-undo">{t("common.undo")}</DeckButton><DeckButton primary onClick={onClose}>{t("common.ok")}</DeckButton></>}>
        <p className="deck-line">{t("crucible.install.doneDetail")}</p>
      </Dialog>
    );
  }
  return (
    <Dialog name="crucible-preview" title={m?.title ?? t("crucible.preview.title")} wide onClose={onClose} locked={busy}
      buttons={<>
        <DeckButton onClick={onClose}>{t("common.cancel")}</DeckButton>
        <button type="button" className={"deck-btn primary" + (ready ? "" : " kcr-deck-wait")} aria-disabled={!ready || undefined} data-deck-autofocus="" data-deck-id="crucible-install"
          data-deck-a={t("crucible.preview.installPlay")} onClick={() => void go()} data-testid="deck-crucible-install">{busy ? t("crucible.deck.installing") : t("crucible.preview.installPlay")}</button>
      </>}>
      <div className="deck-scroll deck-list" data-testid="deck-crucible-preview">
        {q.isLoading ? <p className="deck-line">{t("crucible.deck.loading")}</p> : q.error ? <p className="deck-line"><Word tone="err">{crucibleError(q.error)}</Word></p> : m && <DeckPreviewBody m={m} />}
        {m && m.uploading != null && !wait && <Row id="crucible-wait" title={t("crucible.preview.wait")} sub={t("crucible.preview.stillUploading", { name: m.creator?.name ?? m.title, pct: m.uploading })} onClick={() => setWait(true)} />}
        {m && m.mods.filter((x) => x.optional).map((x) => (
          <Row key={x.id} id={"opt-" + x.id} title={x.name} sub={t("crucible.preview.optionalTag")} lit={!!optional?.[x.id]} a={optional?.[x.id] ? t("crucible.deck.turnOff") : t("crucible.deck.turnOn")}
            onClick={() => setOptional((o) => ({ ...(o ?? {}), [x.id]: !o?.[x.id] }))} />
        ))}
        {m && !m.instance && (
          <>
            <p className="deck-line" data-testid="deck-crucible-warning"><Word tone="warn">{m.source.kind === "file" ? t("crucible.preview.warningFile") : t("crucible.preview.warning")}</Word></p>
            {m.trusted ? <p className="deck-note">{t("crucible.preview.trustedAlready", { name: m.creator?.name ?? "" })}</p> : (
              <>
                <Row id="crucible-trust" title={m.creator ? t("crucible.preview.trust", { name: m.creator.name }) : t("crucible.preview.trustFile")} lit={trust}
                  a={trust ? t("crucible.deck.untick") : t("crucible.deck.tick")} onClick={() => { setTrust((x) => !x); setNote(null); }} />
                {m.creator?.id && <Row id="crucible-always" title={t("crucible.preview.trustAlways", { name: m.creator.name })} lit={always}
                  a={always ? t("crucible.deck.untick") : t("crucible.deck.tick")} onClick={() => setAlways((x) => !x)} />}
              </>
            )}
          </>
        )}
        {note && <p className="deck-line" role="alert" data-testid="deck-crucible-note"><Word tone="warn">{note}</Word></p>}
      </div>
    </Dialog>
  );
}

function DeckPreviewBody({ m }: { m: PreviewModel }) {
  const t = useT();
  const checks = checksLine(m.checks);
  return (
    <>
      <p className="deck-line">{m.creator ? t("crucible.preview.by", { name: m.creator.name }) : t("crucible.preview.fromFile")}{m.version && " · v" + m.version}{m.upvotes != null && " · ▲ " + m.upvotes}</p>
      <p className="deck-line"><Word tone={m.fits ? "ok" : "warn"}>{m.fits ? (m.current ? t("crucible.preview.fits", { build: m.current }) : t("crucible.preview.fitsAny")) : t("crucible.preview.otherBuild", { build: m.builds[0] ?? "", current: m.current ?? "" })}</Word></p>
      {!m.kiln.ok && <p className="deck-line"><Word tone="err">{t("crucible.preview.kilnOld", { version: neededKiln(m) })}</Word></p>}
      {m.uploading == null && <p className="deck-line">{sizeLine(m)}</p>}
      {checks && <p className="deck-line" data-testid="deck-crucible-checks"><Word tone={checks.kind === "problems" ? "err" : checks.kind === "warnings" ? "warn" : checks.kind === "passed" ? "ok" : "idle"}>
        {checks.kind === "passed" ? t("crucible.checks.passed") : checks.kind === "warnings" ? t("crucible.checks.lineWarnings", { count: checks.count })
          : checks.kind === "problems" ? t("crucible.checks.lineProblems", { count: checks.count }) : checks.kind === "pending" ? t("crucible.checks.pending") : t("crucible.checks.lineFailed")}
      </Word></p>}
      <p className="deck-note">{m.instance ? t("crucible.preview.have") : t("crucible.preview.ownProfile")}</p>
      {m.mods.length > 0 && <p className="deck-note">{m.mods.map((x) => x.name + (x.author ? " (" + x.author + ")" : "")).join(", ")}</p>}
    </>
  );
}

/** Mounted once in the Deck UI: the preview that a code, a link or a file opened. */
export function DeckCrucibleHost() {
  useStatusFollowsSignIn();
  const preview = useCrucibleUi((s) => s.preview);
  const openPreview = useCrucibleUi((s) => s.openPreview);
  const close = useCrucibleUi((s) => s.closePreview);
  useCrucibleLinks((target) => openPreview({ kind: "crucible", target }));
  if (!preview) return null;
  return <DeckPreviewDialog key={preview.kind === "file" ? preview.path : preview.target} source={preview} onClose={close} />;
}
