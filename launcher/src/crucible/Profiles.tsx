// Settings > Profiles, for Crucible: a profile made from a pack (an instance) says its pack and version, an update
// waiting (the same update screen as Play), Undo last update (pack_undo) and Uninstall (pack_uninstall: the profile
// goes, its mods stay in the Library); the Crucible card lists the installed packs and the packs the player shared
// (link, code, Copy, "Pack changed" with Update link).
import { useQueryClient, type QueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import * as api from "../api/commands";
import type { CrucibleUpdate, PackInstance, ProfileId } from "../api/commands";
import { useGame, useProfiles } from "../api/hooks";
import type { MenuItem } from "../ui";
import { t, useT } from "../i18n";
import { Button, Card, Modal, fmtAgo, toast } from "../ui";
import EMPTY from "../assets/crucible/empty.webp";
import { anyChange, countsOf, coverFor } from "./code";
import { LinkRows, changeWords, startShare } from "./Share";
import { refreshCrucible, useCrucibleUi, useInstances, useShareRun, useShared, useUpdates } from "./state";

/** The line under an instance's name: its pack and version, when it was installed or updated, an update waiting. */
export function PackLine({ instance, update, profileId }: { instance: PackInstance; update: CrucibleUpdate | null; profileId: ProfileId }) {
  const t = useT();
  const open = useCrucibleUi((s) => s.openUpdate);
  return (
    <span className="k-item-sub" data-testid="crucible-pack-line">
      {t("crucible.profiles.line", { name: instance.name, version: instance.version, ago: fmtAgo(instance.updated ?? instance.installed) })}
      {update && <> · <button type="button" className="k-link" onClick={() => open(profileId)} data-testid="crucible-profile-update">{t("crucible.profiles.updateReady", { version: update.to.version })}</button></>}
    </span>
  );
}

async function undoUpdate(qc: QueryClient, profileId: ProfileId, instance: PackInstance, gameId: string | null) {
  try {
    await api.packUndo(profileId);
    refreshCrucible(qc, gameId);
    toast.ok(t("crucible.update.undone", { name: instance.name, version: instance.previous ?? "" }));
  } catch (e) { toast.error(e, t("crucible.update.undoFailed")); }
}

/** The instance's entries in the row's menu. */
export function packMenu(profileId: ProfileId, instance: PackInstance, update: CrucibleUpdate | null, gameId: string | null, qc: QueryClient, loaded: boolean, uninstall: () => void): MenuItem[] {
  return [
    ...(update ? [{ label: t("crucible.profiles.seeUpdate"), onSelect: () => useCrucibleUi.getState().openUpdate(profileId) }] : []),
    ...(instance.previous ? [{ label: t("crucible.profiles.undoUpdate", { version: instance.previous }), onSelect: () => void undoUpdate(qc, profileId, instance, gameId) }] : []),
    { label: t("crucible.profiles.uninstall"), danger: true, disabled: loaded, onSelect: uninstall },
  ];
}

export function UninstallPackModal({ profileId, instance, onClose }: { profileId: ProfileId; instance: PackInstance; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const { game } = useGame();
  const [busy, setBusy] = useState(false);
  const go = async () => {
    setBusy(true);
    try {
      const r = await api.packUninstall(profileId);
      refreshCrucible(qc, game?.id);
      toast.ok(t("crucible.profiles.uninstalled", { name: r.profile.name }), r.unused.length ? t("crucible.profiles.unused", { count: r.unused.length }) : undefined);
      onClose();
    } catch (e) { toast.error(e); } finally { setBusy(false); }
  };
  return (
    <Modal open onClose={onClose} locked={busy} width={480} title={t("crucible.profiles.uninstallAsk", { name: instance.name })}
      footer={<><Button variant="ghost" onClick={onClose} disabled={busy}>{t("common.cancel")}</Button>
        <Button variant="danger" loading={busy} onClick={() => void go()} data-testid="crucible-uninstall-go">{t("crucible.profiles.uninstall")}</Button></>}>
      <p className="kcr-p">{t("crucible.profiles.uninstallText")}</p>
    </Modal>
  );
}

export function CruciblePacksCard() {
  const t = useT();
  const { game } = useGame();
  const profiles = useProfiles();
  const instances = useInstances();
  const updates = useUpdates(game?.id);
  const open = useCrucibleUi((s) => s.openUpdate);
  const list = instances.data ?? [];
  const name = (pid: string) => profiles.data?.find((p) => p.id === pid)?.name ?? pid;
  return (
    <Card id="crucible" aria-label={t("crucible.profiles.cardTitle")} title={t("crucible.profiles.cardTitle")} sub={t("crucible.profiles.cardSub")}>
      <div className="kcr-label">{t("crucible.profiles.installed")}</div>
      {list.length ? (
        <ul className="kcr-card-list" data-testid="crucible-installed">
          {list.map(([pid, i]) => {
            const u = updates.data?.find((x) => x.profile === pid);
            return (
              <li key={pid}>
                <span className="kcr-thumb" style={{ backgroundImage: "url(" + coverFor(i.crucible?.pack ?? i.pack) + ")" }} aria-hidden />
                <span className="k-stack" style={{ gap: 1, flex: 1, minWidth: 0 }}>
                  <span className="k-item-title k-ellipsis">{i.name} <span className="k-muted">{i.version}</span></span>
                  <span className="k-item-sub">{t("crucible.profiles.asProfile", { name: name(pid) })}</span>
                </span>
                {u && <Button size="s" onClick={() => open(pid)}>{t("crucible.profiles.updateTo", { version: u.to.version })}</Button>}
              </li>
            );
          })}
        </ul>
      ) : <Empty text={t("crucible.profiles.noInstalled")} testid="crucible-no-installed" />}
      <div className="kcr-label" style={{ marginTop: 10 }}>{t("crucible.profiles.shared")}</div>
      <SharedList ids={(profiles.data ?? []).map((p) => p.id)} stamps={Object.fromEntries((profiles.data ?? []).map((p) => [p.id, p.updated]))} name={name} />
    </Card>
  );
}

function Empty({ text, testid }: { text: string; testid: string }) {
  return <div className="kcr-empty" data-testid={testid}><img src={EMPTY} alt="" aria-hidden /><span className="k-muted">{text}</span></div>;
}

/** One query per profile (crucible_shared answers from the launcher's own record). */
function SharedList({ ids, stamps, name }: { ids: string[]; stamps: Record<string, string>; name: (pid: string) => string }) {
  const t = useT();
  const [seen, setSeen] = useState<Record<string, boolean>>({});
  const rows = ids.map((id) => <SharedRow key={id} profileId={id} stamp={stamps[id]} name={name(id)} onKnown={(has) => setSeen((s) => (s[id] === has ? s : { ...s, [id]: has }))} />);
  const any = Object.values(seen).some(Boolean);
  return (
    <>
      <ul className="kcr-card-list" data-testid="crucible-shared-list">{rows}</ul>
      {!any && Object.keys(seen).length === ids.length && <Empty text={t("crucible.profiles.noShared")} testid="crucible-no-shared" />}
    </>
  );
}

function SharedRow({ profileId, stamp, name, onKnown }: { profileId: string; stamp?: string; name: string; onKnown: (has: boolean) => void }) {
  const t = useT();
  const qc = useQueryClient();
  const q = useShared(profileId, stamp);
  const busy = useShareRun((s) => { const r = s.runs[profileId]; return !!r && !r.result && !r.error && !r.pendingSignIn; });
  const s = q.data;
  const known = q.isSuccess || q.isError;
  useEffect(() => { if (known) onKnown(!!s); });
  if (!s) return null;
  const changed = s.changed && anyChange(countsOf(s.changed)) ? s.changed : null;
  return (
    <li className="kcr-shared-row" data-testid="crucible-shared-row" style={{ flexDirection: "column", alignItems: "stretch", gap: 4 }}>
      <span className="k-row" style={{ gap: 8, justifyContent: "space-between" }}>
        <span className="k-item-title">{name} <span className="k-muted">{t("crucible.profiles.sharedVersion", { number: s.version.number })}</span></span>
        {changed && <span className="k-row" style={{ gap: 8 }}><span className="kcr-pill">{t("crucible.share.changedPill")}</span>
          <Button size="s" loading={busy} onClick={() => void startShare(qc, profileId)} title={changeWords(changed)}>{t("crucible.share.updateLink")}</Button></span>}
      </span>
      <LinkRows url={s.pack.url} code={s.pack.code} />
    </li>
  );
}
