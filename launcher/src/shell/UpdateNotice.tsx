// The launcher update in the header: a word beside the tasks (Update, Updating, Restart, Updated) and its notice,
// which drops under the header by itself when an update is found, is ready or was just installed. Each state is one line
// and its buttons; sources, fallbacks and failures go to launcher.log, and a failed download is a short toast.
//   available     "Kiln <version>"        Skip, Update (or Download for a .deb or development build)
//   downloading   "Updating…" and a bar
//   ready         "Ready to restart"       Later, Restart. The launcher restarts by itself after a short countdown
//                                          (Later stops it), unless the game runs or Pack Studio or an editor has unsaved
//                                          work: then it says why it waited and the player restarts. A running task is
//                                          waited for first (shell/updateApply.ts).
//   just updated  "Updated to <version>"   The Kiln runtime the update brought goes into the game by itself while the game
//                                          is closed (the ordinary install: its backup, its refusals; a toast says so),
//                                          then the notice goes. While the game runs: Later, Update game (held) and why.
//                                          A switch back's older Kiln keeps Update game (and Play puts it in).
// Only in builds that update themselves: the Nexus Mods build (app_info.distribution "nexus") has no word, no notice
// and no update feed (api/distribution.ts).
import { useQueryClient } from "@tanstack/react-query";
import { X } from "lucide-react";
import { useEffect, useState, type ReactNode } from "react";
import * as api from "../api/commands";
import type { UpdateStatus } from "../api/commands";
import { useDistribution, useGame, useGameStatus, useRuntimeChanges } from "../api/hooks";
import { useUpdate, useUpdateFeed, useUpdateStore } from "../api/update";
import { buildLabel } from "../api/channel";
import { Button, IconButton, ProgressBar, toast, useTasks, waitTask } from "../ui";
import { useKilnInstall } from "./firstRun";
import { runRuntimeChange } from "./runtimeChange";
import { applyWait, restartHold, runtimeToApply, type ApplyWait } from "./updateApply";
import { useUnsavedKind } from "./unsaved";
import { t, useT } from "../i18n";
import "./update.css";

/** A short error toast; the detail is in launcher.log. */
const failed = (title: string) => toast.error({ code: "internal", message: title });

/** How long the notice counts down before the launcher restarts into a ready update by itself (tests in the browser
 *  mock: window.__kilnRestartDelayMs). */
const restartDelay = () => (window as unknown as { __kilnRestartDelayMs?: number }).__kilnRestartDelayMs ?? 5000;

function wordFor(s: UpdateStatus): { text: string; tone: "info" | "ember" | "ok" } | null {
  if (s.justUpdated) return { text: t("shell.updateNotice.updated"), tone: "ok" };
  if (s.phase === "ready") return { text: t("shell.updateNotice.restart"), tone: "ok" };
  if (s.phase === "downloading") return { text: t("shell.gameState.updating"), tone: "ember" };
  if (s.phase === "available" && s.available) return { text: t("common.update"), tone: "info" };
  return null;
}

/** The header's update word and notice: direct builds only (none in the Nexus build, nor before app_info answers). */
export function UpdateTray() {
  return useDistribution() === "direct" ? <SelfUpdateTray /> : null;
}

function SelfUpdateTray() {
  const t = useT();
  useUpdateFeed();
  const s = useUpdate();
  const open = useUpdateStore((x) => x.open);
  const setOpen = useUpdateStore((x) => x.setOpen);
  // both run whether the notice is open or not: closing it (Later) does not stop them
  const plan = useRestartAfterUpdate(s);
  const apply = useApplyAfterUpdate(s);
  useEffect(() => {
    if (!open) return;
    const esc = (e: KeyboardEvent) => { if (e.key === "Escape") setOpen(false); };
    document.addEventListener("keydown", esc);
    return () => document.removeEventListener("keydown", esc);
  }, [open, setOpen]);
  const word = s && wordFor(s);
  if (!s || !word) return null;
  return (
    <div className="k-upd">
      <button type="button" className={`k-upd-btn k-tone-${word.tone}`} onClick={() => setOpen(!open)} aria-expanded={open} aria-haspopup="dialog"
        aria-label={t("shell.updateNotice.trayAria", { word: word.text })}>
        <span className="k-sc">{word.text}</span>
      </button>
      {open && <UpdateCard s={s} plan={plan} apply={apply} onClose={() => setOpen(false)} />}
    </div>
  );
}

// ---------------------------------------------------------------- the restart, by itself

interface RestartPlan {
  /** Seconds until the launcher restarts by itself; null while it does not count down. */
  left: number | null;
  /** Why it waits, in words; null when it does not. */
  why: string | null;
  /** The task Restart waits for (the button is held while it runs). */
  task: string | null;
  /** No restart by itself for this update (Later, the notice dismissed). */
  stop: () => void;
}

/** The ready update the player started (not a channel switch: its dialog restarts): counts down and restarts, unless the
 *  game runs or there is unsaved work, which holds it for good (the player restarts); a running task is waited for. */
function useRestartAfterUpdate(s: UpdateStatus | null): RestartPlan {
  const tasks = useTasks((x) => x.tasks);
  const order = useTasks((x) => x.order);
  const blocking = order.map((id) => tasks[id]).find((x) => x && (x.state === "running" || x.state === "queued") && x.kind !== "update_install" && x.kind !== "update_switch");
  const { game } = useGame();
  const gs = useGameStatus(game?.id);
  const unsaved = useUnsavedKind();
  const hold = restartHold({ running: !!gs.data?.running, unsaved, task: blocking?.title ?? null });
  // a game whose status is not known yet: no countdown until it is (never a restart under a game that may run)
  const unknown = !!game && !gs.data;
  const version = s?.phase === "ready" && s.ready && !s.switching ? s.ready.version : null;
  // per ready version: held (the game or unsaved work held it: it stays with the player) or stopped (Later, a failure)
  const [decided, setDecided] = useState<Partial<Record<string, "held" | "stopped">>>({});
  const mode = version ? decided[version] ?? "auto" : null;
  const holdKind = hold?.kind ?? null;
  useEffect(() => {
    if (version && mode === "auto" && (holdKind === "game" || holdKind === "unsaved")) setDecided((d) => ({ ...d, [version]: "held" }));
  }, [version, mode, holdKind]);
  const counting = !!version && mode === "auto" && !hold && !unknown;
  const [deadline, setDeadline] = useState<number | null>(null);
  useEffect(() => { setDeadline(counting ? Date.now() + restartDelay() : null); }, [counting, version]);
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (deadline === null || !version) return;
    const tick = setInterval(() => setNow(Date.now()), 250);
    const go = setTimeout(() => {
      api.updateRestart().catch(() => {
        failed(t("shell.channelSwitch.restartFailed"));
        setDecided((d) => ({ ...d, [version]: "stopped" }));
      });
    }, Math.max(0, deadline - Date.now()));
    return () => { clearInterval(tick); clearTimeout(go); };
  }, [deadline, version]);
  const why = !version || mode === "stopped" ? null
    : hold?.kind === "game" ? t("shell.updateNotice.holdGame")
    : hold?.kind === "unsaved" ? (hold.who === "studio" ? t("shell.updateNotice.holdStudio") : t("shell.updateNotice.holdEditor"))
    : hold?.kind === "task" ? t("shell.updateNotice.waitingFor", { task: hold.title }) : null;
  return {
    left: deadline !== null ? Math.max(0, Math.ceil((deadline - now) / 1000)) : null,
    why,
    task: blocking?.title ?? null,
    stop: () => { if (version) setDecided((d) => ({ ...d, [version]: "stopped" })); },
  };
}

// ---------------------------------------------------------------- the runtime the update brought, by itself

interface ApplyPlan {
  /** The Kiln that goes into the game by itself (the launcher's newer one); null: none. */
  next: string | null;
  /** Why it has not gone in yet. */
  wait: ApplyWait;
  /** A runtime change of this game runs (this one or another). */
  applying: boolean;
}

/** One try per update, game and runtime in this session: a failure keeps Update game (and its toast says why). */
const applied = new Set<string>();

/** After the restart into an update: the runtime it brought into the game, while the game is closed. */
function useApplyAfterUpdate(s: UpdateStatus | null): ApplyPlan {
  const qc = useQueryClient();
  const { game } = useGame();
  const to = s?.justUpdated?.to ?? null;
  const gs = useGameStatus(game?.id);
  const k = useKilnInstall(to ? game : null);
  const change = useRuntimeChanges((x) => (game ? x.last[game.id] : undefined));
  const next = to ? runtimeToApply(gs.data) : null;
  const wait = to && game ? applyWait(gs.data) : null;
  const applying = change?.state === "running";
  const bundles = k.bundles;
  useEffect(() => {
    if (!to || !game || !next || wait || applying) return;
    const key = `${to}|${game.id}|${next}`;
    if (applied.has(key)) return;
    applied.add(key);
    void runRuntimeChange(qc, game, bundles, t("shell.runtimeChange.updateTo", { version: next }), next, t("shell.play.kilnInstalled", { version: next }),
      () => api.runtimeInstall(game.id, next, {}), t("shell.updateNotice.appliedLead", { build: buildLabel(to) }))
      .then((ok) => { if (ok) api.updateAck().catch(() => {}); });
  }, [to, game, next, wait, applying, bundles, qc]);
  return { next, wait, applying };
}

// ---------------------------------------------------------------- the notice

function Card({ title, onDismiss, children }: { title: string; onDismiss: () => void; children?: ReactNode }) {
  const t = useT();
  return (
    <div className="k-upd-pop" role="dialog" aria-label={title}>
      <div className="k-upd-head">
        <span className="k-upd-title">{title}</span>
        <IconButton label={t("common.dismiss")} size="s" icon={<X />} onClick={onDismiss} />
      </div>
      {children}
    </div>
  );
}

function UpdateCard({ s, plan, apply, onClose }: { s: UpdateStatus; plan: RestartPlan; apply: ApplyPlan; onClose: () => void }) {
  const t = useT();
  if (s.justUpdated) return <JustUpdated s={s} apply={apply} onClose={onClose} />;
  if (s.phase === "ready") return <ReadyCard plan={plan} onClose={onClose} />;
  if (s.phase === "downloading") {
    const p = s.progress;
    return (
      <Card title={t("shell.updateNotice.updating")} onDismiss={onClose}>
        <ProgressBar value={p?.total ? p.received / p.total : null} />
      </Card>
    );
  }
  return <Offer s={s} onClose={onClose} />;
}

function Offer({ s, onClose }: { s: UpdateStatus; onClose: () => void }) {
  const t = useT();
  const [busy, setBusy] = useState(false);
  const a = s.available;
  if (!a) return <Card title={t("shell.updateNotice.upToDate")} onDismiss={onClose} />;
  const update = async () => {
    setBusy(true);
    try {
      await waitTask(await api.updateInstall());
    } catch {
      failed(t("shell.updateNotice.updateFailed"));
    } finally {
      setBusy(false);
    }
  };
  const skip = () => api.updateSkip(a.version).then(onClose, () => failed(t("shell.updateNotice.skipFailed")));
  return (
    <Card title={t("shell.updateNotice.offerTitle", { build: buildLabel(a.version) })} onDismiss={onClose}>
      <div className="k-upd-actions">
        <Button variant="ghost" onClick={skip}>{t("shell.updateNotice.skip")}</Button>
        {s.install.inPlace
          ? <Button variant="primary" loading={busy} onClick={update}>{t("common.update")}</Button>
          : <Button variant="primary" onClick={() => api.appOpenUrl(a.page || s.page).catch(() => failed(t("shell.channelSwitch.openPageFailed")))}>{t("shell.updateNotice.download")}</Button>}
      </div>
    </Card>
  );
}

function ReadyCard({ plan, onClose }: { plan: RestartPlan; onClose: () => void }) {
  const t = useT();
  const [busy, setBusy] = useState(false);
  const later = () => { plan.stop(); onClose(); };
  const restart = async () => {
    setBusy(true);
    try { await api.updateRestart(); } catch { failed(t("shell.channelSwitch.restartFailed")); setBusy(false); }
  };
  return (
    <Card title={t("shell.updateNotice.readyTitle")} onDismiss={later}>
      {plan.left !== null && <span className="k-hint" role="status" data-testid="update-restart-in">{t("shell.updateNotice.restartingIn", { seconds: plan.left })}</span>}
      {plan.why && <span className="k-hint" data-testid="update-restart-why">{plan.why}</span>}
      <div className="k-upd-actions">
        <Button variant="ghost" onClick={later}>{t("shell.channelSwitch.later")}</Button>
        <Button variant="primary" loading={busy} disabled={!!plan.task} title={plan.task ? t("shell.updateNotice.waitingFor", { task: plan.task }) : undefined} onClick={restart}>{t("shell.updateNotice.restart")}</Button>
      </div>
    </Card>
  );
}

function JustUpdated({ s, apply, onClose }: { s: UpdateStatus; apply: ApplyPlan; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const { game } = useGame();
  const gs = useGameStatus(game?.id);
  const k = useKilnInstall(game);
  const u = s.justUpdated!;
  const rt = gs.data?.runtime;
  const running = !!gs.data?.running;
  // the launcher ships a runtime: after an update it is in the launcher, not yet in the game; after a switch back from a
  // test build it is older than the game's, which came with the test build (InstallState.switchBack)
  const back = rt?.installed ? rt.switchBack ?? null : null;
  const next = rt?.installed ? rt.upgradeAvailable ?? back?.to ?? null : null;
  const ack = () => { api.updateAck().catch(() => {}); onClose(); };
  const updateGame = async () => {
    if (!game || !next) return;
    // no explicit parts: the backend keeps the game's parts and adds the ones that are new or became defaults, unless
    // the player turned them off (passing the receipt's list here froze it and skipped new parts). A DLL Windows refused
    // (an antivirus, inbox 92) is said in its plain words (runRuntimeChange).
    const ok = await runRuntimeChange(qc, game, k.bundles, t("shell.runtimeChange.updateTo", { version: next }), next, t("shell.play.kilnInstalled", { version: next }),
      () => api.runtimeInstall(game.id, next, {}));
    if (ok) ack();
  };
  return (
    <Card title={t("shell.updateNotice.updatedTo", { build: buildLabel(u.to) })} onDismiss={ack}>
      {game && apply.wait === "loading" && <span className="k-hint" data-testid="update-game-loading">{t("shell.updateNotice.lookingAtGame")}</span>}
      {apply.applying && next && <span className="k-hint" role="status" data-testid="update-game-applying">{t("shell.updateNotice.applying", { version: next })}</span>}
      {!apply.applying && apply.next && apply.wait === "running" && <span className="k-hint" data-testid="update-game-waiting">{t("shell.updateNotice.afterGame", { version: apply.next })}</span>}
      {!apply.applying && apply.next && apply.wait === "steam" && <span className="k-hint" data-testid="update-game-waiting">{t("shell.updateNotice.afterSteam", { version: apply.next })}</span>}
      {back && !rt?.upgradeAvailable && <span className="k-hint" data-testid="switch-back-runtime">{t("shell.updateNotice.switchBack", { from: back.from, to: back.to })}</span>}
      {next && !apply.applying && (
        <div className="k-upd-actions">
          <Button variant="ghost" onClick={onClose}>{t("shell.channelSwitch.later")}</Button>
          <Button variant="primary" disabled={running} title={running ? t("shell.updateNotice.closeGame") : undefined} onClick={updateGame}>{t("shell.updateNotice.updateGame")}</Button>
        </div>
      )}
    </Card>
  );
}
