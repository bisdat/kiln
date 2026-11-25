// Settings, Profiles: profiles as a background feature. One profile is always loaded per game (the main screen's
// switches show it; the launcher makes "My mods" by itself), and here the player keeps others: every saved profile with
// its mod count over every kind and the server it is for, which one is loaded, Load, "Save current as..." (a copy of
// the loaded one), Rename, the folders and notes dialog, Delete (not the loaded one), and "No profile" (the game folder
// decides). The modpack flow's "Server: <name>" profiles are listed here too; that flow loads them itself. Crucible's
// packs are a group of their own under it (pages/settings/sections/Mods.tsx).
import { useMutation, useQueryClient } from "@tanstack/react-query";
import { MoreHorizontal } from "lucide-react";
import { useState } from "react";
import * as api from "../api/commands";
import type { ProfileSummary } from "../api/commands";
import { otherFolderProfile, otherFolderTitle } from "../shell/dataFolderText";
import { invalidateGame, qk, showActive, useActiveProfile, useGame, useGameStatus, useProfile, useProfiles } from "../api/hooks";
import { totalCount } from "../api/kinds";
import { ProfileSettingsModal } from "../pages/profiles/ProfileLayout";
import { dropinKinds, setDropins } from "../pages/profiles/dropins";
import { forgetProfileEdits, useProfileEdit } from "../pages/profiles/useProfileEdit";
import { notifyLibraryChanged } from "../shell/libraryChanged";
import { notifyActivation } from "../shell/notices";
import { rich, useT } from "../i18n";
import { PackLine, UninstallPackModal, packMenu } from "../crucible/Profiles";
import { useCrucibleOn, useInstances, useUpdates } from "../crucible/state";
import type { PackInstance } from "../api/commands";
import { Button, Card, ConfirmDanger, EmptyState, Field, Menu, Modal, Skeleton, TextInput, errorText, fmtAgo, notifyPlacementCleanup, toast } from "../ui";

// Capture intent when the dialog opens: a query/selection change must never turn a copy into an empty profile.
type SaveProfile = { kind: "empty" } | { kind: "copy"; from: ProfileSummary; gameId: string | null };

export function ProfilesCard() {
  const t = useT();
  const qc = useQueryClient();
  const { game } = useGame();
  const status = useGameStatus(game?.id);
  const profiles = useProfiles();
  const active = useActiveProfile(game?.id);
  const [saveAs, setSaveAs] = useState<SaveProfile | null>(null);
  const [rename, setRename] = useState<ProfileSummary | null>(null);
  const [folders, setFolders] = useState<string | null>(null);
  const [del, setDel] = useState<ProfileSummary | null>(null);
  // (dev.30) Crucible packs: a profile made from a pack is an instance (its pack, version, update, Undo, Uninstall)
  const crucible = useCrucibleOn();
  const instances = useInstances(crucible);
  const updates = useUpdates(game?.id, crucible);
  const [uninstall, setUninstall] = useState<{ id: string; instance: PackInstance } | null>(null);
  const running = !!status.data?.running;
  const installed = !!status.data?.runtime.installed;
  const loaded = active.data?.profileId ?? null;
  const list = profiles.data ?? [];
  const nameOf = (id: string | null) => (id ? list.find((p) => p.id === id)?.name ?? id : t("profilesSettings.noProfile"));
  const load = useMutation({
    mutationFn: (pid: string | null) => (pid ? api.profileActivate(pid, game!.id) : api.profileDeactivate(game!.id).then((r) => { notifyPlacementCleanup(r); return null; })),
    // the loaded one changes at once (the activation answers after the backend has written it); a failure puts it back
    onMutate: (pid) => showActive(qc, game!.id, pid),
    onSuccess: (r, pid) => {
      qc.setQueryData(qk.active(game!.id), r);
      invalidateGame(qc, game!.id);
      toast.ok(pid ? t("profilesSettings.loadedToast", { name: nameOf(pid) }) : t("profilesSettings.noneLoadedToast"), pid ? (status.data?.start === "direct" ? t("profilesSettings.loadedDetailDirect") : t("profilesSettings.loadedDetail")) : t("profilesSettings.noneLoadedDetail"));
      notifyActivation(r);
    },
    onError: (e, pid, undo) => { undo?.(); if (!notifyLibraryChanged(qc, e, () => load.mutate(pid))) toast.error(e); },
  });
  const blocked = !game || !installed || running || load.isPending;
  const why = !game ? t("profilesSettings.findGame") : !installed ? t("profilesSettings.installKiln") : running ? t("profilesSettings.closeGame") : undefined;
  const cur = list.find((p) => p.id === loaded) ?? null;
  const ready = profiles.isSuccess && !profiles.isFetching && (!game || (active.isSuccess && !active.isFetching)) && !load.isPending;
  const empty = ready && !loaded;
  const canSave = ready && (!!cur || !loaded);

  return (
    <Card id="profiles" aria-label={t("profilesSettings.title")} title={t("profilesSettings.title")}
      sub={status.data?.otherFolderProfile
        ? <span className="k-tone-warn" title={otherFolderTitle(status.data.otherFolderProfile)} data-testid="other-folder-profile">{otherFolderProfile()}</span>
        : cur ? rich("profilesSettings.subLoaded", { name: <b>{cur.name}</b> }) : t("profilesSettings.sub")}
      actions={<Button size="s" disabled={!canSave} title={canSave ? undefined : t("profilesSettings.loadFirst")}
        onClick={() => { if (canSave) setSaveAs(cur ? { kind: "copy", from: cur, gameId: game?.id ?? null } : { kind: "empty" }); }}>
        {t(empty ? "profilesSettings.createEmpty" : "profilesSettings.saveAs")}</Button>}>
      {profiles.isLoading ? <Skeleton lines={3} /> : profiles.error ? <EmptyState title={t("profilesSettings.unreadable")} hint={errorText(profiles.error)} /> : (
        <ul className="k-list k-profiles" aria-label={t("profilesSettings.saved")}>
          {list.map((p) => {
            const n = totalCount(p.counts);
            const here = p.id === loaded;
            const inst = crucible ? instances.byProfile.get(p.id) ?? null : null;
            const upd = inst ? updates.data?.find((u) => u.profile === p.id) ?? null : null;
            return (
              <li key={p.id} className="k-list-item" style={{ padding: "8px 0" }}>
                <span className="k-stack" style={{ gap: 1, flex: 1, minWidth: 0 }}>
                  <span className="k-item-title k-ellipsis">{p.name}{inst && <span className="kcr-tag" data-testid="crucible-tag">{t("crucible.profiles.tag")}</span>}</span>
                  {inst && <PackLine instance={inst} update={upd} profileId={p.id} />}
                  <span className="k-item-sub">{p.server
                    ? t("profilesSettings.lineServer", { count: n, server: p.server.name ?? p.server.id, ago: fmtAgo(p.updated) })
                    : t("profilesSettings.line", { count: n, ago: fmtAgo(p.updated) })}</span>
                </span>
                {here ? <span className="k-pill k-tone-gold">{t("profilesSettings.loaded")}</span> : (
                  <Button size="s" disabled={blocked} title={why} loading={load.isPending && load.variables === p.id} onClick={() => load.mutate(p.id)}>{t("profilesSettings.load")}</Button>
                )}
                <Menu label={t("profilesSettings.more", { name: p.name })} icon={<MoreHorizontal />} size="s" items={[
                  { label: t("profilesSettings.rename"), onSelect: () => setRename(p) },
                  { label: t("profilesSettings.folders"), onSelect: () => setFolders(p.id) },
                  ...(inst ? packMenu(p.id, inst, upd, game?.id ?? null, qc, p.active.length > 0, () => setUninstall({ id: p.id, instance: inst })) : []),
                  ...(inst ? [] : [{ label: t("common.delete"), danger: true, disabled: p.active.length > 0, onSelect: () => setDel(p) }]),
                ]} />
              </li>
            );
          })}
          <li className="k-list-item" style={{ padding: "8px 0" }}>
            <span className="k-stack" style={{ gap: 1, flex: 1, minWidth: 0 }}>
              <span className="k-item-title">{t("profilesSettings.noProfile")}</span>
              <span className="k-item-sub">{t("profilesSettings.noProfileSub")}</span>
            </span>
            {game && !loaded && active.isSuccess ? <span className="k-pill k-tone-gold">{t("profilesSettings.loaded")}</span> : (
              <Button size="s" disabled={blocked || !loaded} title={why} loading={load.isPending && load.variables === null} onClick={() => load.mutate(null)}>{t("profilesSettings.load")}</Button>
            )}
            <span style={{ width: 32, flex: "none" }} aria-hidden />
          </li>
        </ul>
      )}
      {saveAs && <SaveAsModal creation={saveAs} onClose={() => setSaveAs(null)} onSaved={(p) => {
        qc.invalidateQueries({ queryKey: qk.profiles });
        if (saveAs.kind === "empty") {
          toast.ok(t("profilesSettings.createdLocal", { name: p.name }), t("profilesSettings.createdLocalDetail"));
        } else {
          toast.ok(t("profilesSettings.savedAs", { name: p.name }), cur ? t("profilesSettings.staysLoaded", { name: cur.name }) : t("profilesSettings.loadedStaysLoaded"),
            { actions: game && game.id === saveAs.gameId && !status.data?.otherFolderProfile ? [{ label: t("profilesSettings.loadIt"), onClick: () => load.mutate(p.id) }] : [] });
        }
      }} />}
      <RenameModal profile={rename} onClose={() => setRename(null)} />
      {uninstall && <UninstallPackModal profileId={uninstall.id} instance={uninstall.instance} onClose={() => setUninstall(null)} />}
      {folders && <FoldersModal id={folders} onClose={() => setFolders(null)} />}
      <ConfirmDanger open={!!del} onClose={() => setDel(null)} title={t("profilesSettings.deleteAsk", { name: del?.name ?? "" })} message={t("profilesSettings.deleteMessage")} word={del?.id ?? ""}
        action={t("common.delete")} onConfirm={async () => {
          try {
            const id = del!.id;
            await api.profileDelete(id);
            // nothing of it stays: a profile made again under the same name (the same id) starts from its own files
            forgetProfileEdits(id);
            qc.removeQueries({ queryKey: qk.profile(id) });
            qc.removeQueries({ queryKey: ["profile_resolve", id] });
            toast.ok(t("profilesSettings.deleted"));
            qc.invalidateQueries({ queryKey: qk.profiles });
          }
          catch (e) { toast.error(e); throw e; }
        }} />
    </Card>
  );
}

function SaveAsModal({ creation, onClose, onSaved }: { creation: SaveProfile; onClose: () => void; onSaved: (p: api.Profile) => void }) {
  const t = useT();
  const from = creation.kind === "copy" ? creation.from : null;
  const [name, setName] = useState("");
  const save = useMutation({
    mutationFn: () => api.profileCreate(name.trim(), creation.kind === "copy" ? creation.from.id : null),
    onSuccess: (p) => { setName(""); onClose(); onSaved(p); },
    onError: (e) => toast.error(e),
  });
  return (
    <Modal open onClose={onClose} locked={save.isPending} title={t(from ? "profilesSettings.saveAsTitle" : "profilesSettings.createEmptyTitle")} width={480}
      footer={<><Button variant="ghost" disabled={save.isPending} onClick={onClose}>{t("common.cancel")}</Button><Button variant="primary" disabled={!name.trim()} loading={save.isPending} onClick={() => save.mutate()}>{t("common.save")}</Button></>}>
      <Field label={t("profilesSettings.name")} hint={from ? t("profilesSettings.copyOf", { name: from.name }) : t("profilesSettings.createEmptyHint")} htmlFor="pf-save-as">
        <TextInput id="pf-save-as" data-autofocus value={name} onChange={(e) => setName(e.target.value)} placeholder={from ? t("profilesSettings.copyName", { name: from.name }) : ""}
          onKeyDown={(e) => { if (e.key === "Enter" && name.trim() && !save.isPending) save.mutate(); }} />
      </Field>
    </Modal>
  );
}

function RenameModal({ profile, onClose }: { profile: ProfileSummary | null; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const [name, setName] = useState("");
  const [seen, setSeen] = useState<string | null>(null);
  if ((profile?.id ?? null) !== seen) { setSeen(profile?.id ?? null); setName(profile?.name ?? ""); }
  const save = useMutation({
    mutationFn: async () => { const p = await api.profileGet(profile!.id); return api.profileSave({ ...p, name: name.trim() }); },
    onSuccess: (p) => { qc.setQueryData(qk.profile(p.id), p); qc.invalidateQueries({ queryKey: qk.profiles }); toast.ok(t("profilesSettings.renamed", { name: p.name })); onClose(); },
    onError: (e) => toast.error(e),
  });
  return (
    <Modal open={!!profile} onClose={onClose} title={t("profilesSettings.renameTitle", { name: profile?.name ?? "" })} width={480}
      footer={<><Button variant="ghost" onClick={onClose}>{t("common.cancel")}</Button><Button variant="primary" disabled={!name.trim() || name.trim() === profile?.name} loading={save.isPending} onClick={() => save.mutate()}>{t("profilesSettings.rename")}</Button></>}>
      <Field label={t("profilesSettings.name")} htmlFor="pf-rename">
        <TextInput id="pf-rename" data-autofocus value={name} onChange={(e) => setName(e.target.value)} onKeyDown={(e) => { if (e.key === "Enter" && name.trim()) save.mutate(); }} />
      </Field>
    </Modal>
  );
}

/** The profile's folders and notes (name, mod data, drop-in folders, notes): the dialog the mods' views open too. */
function FoldersModal({ id, onClose }: { id: string; onClose: () => void }) {
  const q = useProfile(id);
  const { update } = useProfileEdit(id);
  const p = q.data;
  if (!p) return null;
  return (
    <ProfileSettingsModal open onClose={onClose} name={p.name} notes={p.notes} dropins={dropinKinds(p)} dataDir={p.dataDir}
      onSave={(v) => { update((x) => { x.name = v.name; x.notes = v.notes; setDropins(x, v.dropins); x.dataDir = v.dataDir; }); onClose(); }} />
  );
}
