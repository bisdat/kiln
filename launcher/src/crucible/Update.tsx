// Pack updates for an instance (DESIGN 1.5): the Play banner "Ember run 3 -> 4 by Emberfox: 1 added, 2 updated,
// 1 removed. [See changes] [x]", and the update screen: what the new version adds, updates and removes, the creator's
// words, what of the player's own is kept, and each conflict with "take the pack's" / "keep mine". crucible_update's
// "conflict" error carries a fresh plan: the screen shows it again. The toast after an update has Undo (pack_undo).
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import * as api from "../api/commands";
import type { CrucibleUpdate, GameInstall, PackChoice, PackUpdatePlan, ProfileId } from "../api/commands";
import { qk, useGame, useGameStatus, useLibrary, useProfiles } from "../api/hooks";
import { t, useT } from "../i18n";
import { Banner, Button, Modal, Skeleton, Spinner, fmtBytes, runTask, toast } from "../ui";
import { usePlayControl } from "../shell/play";
import { crk, crucibleError, refreshCrucible, useCrucibleOn, useCrucibleUi, useUpdates } from "./state";

/** "3 -> 4" when the numbers are known, else the versions. */
export const fromTo = (u: CrucibleUpdate) => (u.from.number != null ? u.from.number + " → " + u.to.number : u.from.version + " → " + u.to.version);

/** The banner's sentence. */
export function updateLine(u: CrucibleUpdate): string {
  const head = u.by ? t("crucible.banner.headBy", { name: u.name, change: fromTo(u), by: u.by }) : t("crucible.banner.head", { name: u.name, change: fromTo(u) });
  if (!u.counts) return t("crucible.banner.ready", { head });
  const parts = [u.counts.added ? t("crucible.change.addedWord", { count: u.counts.added }) : "", u.counts.updated ? t("crucible.change.updatedWord", { count: u.counts.updated }) : "",
    u.counts.removed ? t("crucible.change.removedWord", { count: u.counts.removed }) : ""].filter(Boolean);
  return parts.length ? t("crucible.banner.line", { head, changes: parts.join(", ") }) : t("crucible.banner.ready", { head });
}

const DISMISS = "kiln.crucible.dismissed";
const dismissed = (): string[] => { try { return JSON.parse(localStorage.getItem(DISMISS) ?? "[]"); } catch { return []; } };

/** Play's banner for the loaded profile's pack update. */
export function CrucibleUpdateBanner({ game, profileId }: { game: GameInstall; profileId: ProfileId | null }) {
  const t = useT();
  const on = useCrucibleOn();
  const updates = useUpdates(game.id, on && !!profileId);
  const open = useCrucibleUi((s) => s.openUpdate);
  const [gone, setGone] = useState(dismissed);
  const u = updates.data?.find((x) => x.profile === profileId);
  if (!on || !u || gone.includes(u.profile + "@" + u.to.versionId)) return null;
  const dismiss = () => { const next = [...dismissed(), u.profile + "@" + u.to.versionId].slice(-50); localStorage.setItem(DISMISS, JSON.stringify(next)); setGone(next); };
  return (
    <div className="k-hero-banner" data-testid="crucible-update-banner">
      <Banner tone="info" title={t("crucible.banner.title")} actions={<>
        <Button size="s" onClick={() => open(u.profile)} data-testid="crucible-see-changes">{t("crucible.banner.seeChanges")}</Button>
        <button type="button" className="k-link" aria-label={t("common.dismiss")} onClick={dismiss} data-testid="crucible-banner-dismiss">×</button>
      </>}>{updateLine(u)}</Banner>
    </div>
  );
}

const show = (v: unknown) => (typeof v === "string" ? v : JSON.stringify(v));

export function UpdateModal({ profileId }: { profileId: ProfileId }) {
  const t = useT();
  const qc = useQueryClient();
  const close = useCrucibleUi((s) => s.closeUpdate);
  const { game } = useGame();
  const status = useGameStatus(game?.id);
  const ctl = usePlayControl();
  const profiles = useProfiles();
  const lib = useLibrary(null);
  const q = useQuery({ queryKey: crk.plan(profileId), queryFn: () => api.crucibleUpdatePlan(profileId), retry: false, staleTime: 60_000 });
  const [plan, setPlan] = useState<PackUpdatePlan | null>(null);
  const [choice, setChoice] = useState<Record<string, PackChoice>>({});
  const [busy, setBusy] = useState(false);
  useEffect(() => { if (q.data && !plan) setPlan(q.data.plan); }, [q.data, plan]);
  const name = profiles.data?.find((p) => p.id === profileId)?.name ?? profileId;
  const active = qc.getQueryData<api.Activation | null>(qk.active(game?.id ?? ""))?.profileId === profileId;
  const modName = (id: string) => lib.data?.find((e) => e.id === id)?.name ?? id;
  const conflicts = plan?.files.flatMap((f) => f.conflicts.map((c) => ({ file: f.path, ...c, key2: f.path + "#" + c.key }))) ?? [];
  const keptKeys = plan?.files.reduce((s, f) => s + f.kept.length, 0) ?? 0;
  const yours = plan ? plan.mods.yours.length + plan.mods.keptSwitches.length : 0;
  const open = conflicts.filter((c) => !choice[c.key2]);
  const running = !!status.data?.running;
  const go = async () => {
    if (!plan || !q.data || !game || open.length) return;
    setBusy(true);
    try {
      const r = await runTask<api.CrucibleUpdateResult>(() => api.crucibleUpdate(game.id, { profile: profileId, versionId: q.data!.versionId, consent: true, resolve: choice }), { quiet: true });
      refreshCrucible(qc, game.id);
      const undo = async () => {
        try { await api.packUndo(profileId); refreshCrucible(qc, game.id); toast.ok(t("crucible.update.undone", { name, version: plan.from })); }
        catch (e) { toast.error(e, t("crucible.update.undoFailed")); }
      };
      toast.ok(t("crucible.update.done", { name, version: r.instance.version }), t("crucible.update.doneDetail"), { actions: [{ label: t("common.undo"), onClick: () => void undo() }], ms: 12_000 });
      close();
      if (active && ctl.canPlay) ctl.play("profile");
    } catch (e) {
      const k = api.toKilnError(e);
      if (k.code === "conflict" && k.detail && typeof k.detail === "object" && "files" in (k.detail as object)) {
        setPlan(k.detail as PackUpdatePlan);
        toast.info(t("crucible.update.needsChoice"));
      } else toast.error(crucibleError(k), t("crucible.update.failed", { name }));
    } finally { setBusy(false); }
  };
  return (
    <Modal open onClose={close} locked={busy} width={620} title={plan ? t("crucible.update.title", { name, from: plan.from, to: plan.to }) : t("crucible.update.titleLoading", { name })}
      footer={<>
        <Button variant="ghost" onClick={close} disabled={busy} data-testid="crucible-not-now">{t("crucible.update.notNow")}</Button>
        <Button variant="primary" loading={busy} disabled={!plan || open.length > 0 || running} title={running ? t("crucible.err.gameRunning") : open.length ? t("crucible.update.chooseFirst") : undefined}
          onClick={() => void go()} data-testid="crucible-update-go">{active ? t("crucible.update.updatePlay") : t("crucible.update.update")}</Button>
      </>}>
      <div className="kcr-update" data-testid="crucible-update">
        {q.isLoading ? <div className="k-row" style={{ gap: 10 }}><Spinner /><span className="k-muted">{t("crucible.update.getting")}</span></div>
          : q.error ? <Banner tone="err" title={t("crucible.update.cantRead")}>{crucibleError(q.error)}</Banner>
          : plan && (
            <>
              {plan.changes && <p className="kcr-p kcr-quote">{plan.changes}</p>}
              <ul className="kcr-diff" data-testid="crucible-diff">
                {plan.mods.added.map((m) => <li key={"a" + m.id} className="add"><b>+</b> {modName(m.id)} {m.version}</li>)}
                {plan.mods.updated.map((m) => <li key={"u" + m.id} className="upd"><b>~</b> {modName(m.id)} {m.from} → {m.to}</li>)}
                {plan.mods.removed.map((m) => <li key={"r" + m.id} className="rem"><b>−</b> {t("crucible.update.removedKept", { name: modName(m.id) })}</li>)}
                {plan.layer.changed && <li className="upd"><b>~</b> {t("crucible.update.packData")}</li>}
              </ul>
              {(keptKeys > 0 || yours > 0) && <p className="kcr-p k-muted" data-testid="crucible-kept">{t("crucible.update.kept", { settings: keptKeys, mods: yours })}</p>}
              {q.data && q.data.download > 0 && <p className="k-hint">{t("crucible.update.download", { size: fmtBytes(q.data.download) })}</p>}
              {conflicts.length > 0 && (
                <div className="kcr-conflicts" data-testid="crucible-conflicts">
                  <div className="kcr-label">{t("crucible.update.needsYou", { count: conflicts.length })}</div>
                  {conflicts.map((c) => (
                    <fieldset key={c.key2} className="kcr-conflict">
                      <legend><span className="k-mono">{c.file}</span> “{c.key}”</legend>
                      <p className="k-hint">{t("crucible.update.both", { pack: show(c.pack), mine: show(c.mine) })}</p>
                      <label className="kcr-tick"><input type="radio" name={c.key2} checked={choice[c.key2] === "pack"} onChange={() => setChoice((x) => ({ ...x, [c.key2]: "pack" }))} data-testid="crucible-take-pack" />
                        <span>{t("crucible.update.takePack", { value: show(c.pack) })}</span></label>
                      <label className="kcr-tick"><input type="radio" name={c.key2} checked={choice[c.key2] === "mine"} onChange={() => setChoice((x) => ({ ...x, [c.key2]: "mine" }))} data-testid="crucible-keep-mine" />
                        <span>{t("crucible.update.keepMine", { value: show(c.mine) })}</span></label>
                    </fieldset>
                  ))}
                </div>
              )}
              {running && <p className="k-hint">{t("crucible.err.gameRunning")}</p>}
            </>
          )}
        {q.isLoading && <Skeleton lines={3} />}
      </div>
    </Modal>
  );
}
