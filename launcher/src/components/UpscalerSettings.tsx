// The vendor upscaler files (dev.29; out of preview in dev.29.18, when phase B runs them behind the game's FSR 3
// option): AMD FSR and Intel XeSS, which Kiln downloads only when the player asks, one row per component of the
// launcher's manifest, in Settings, Graphics under the upscaler choice (components/GraphicsSettings.tsx), for a client
// only. A first download of a vendor's licence text opens the licence dialog (what, size, host, where it goes, the text
// in full, the privacy line; "I agree, download"); later ones start from the row, which says the size and the host.
// Downloads run as tasks (progress, Cancel); a failed one shows its reason with Try again. Remove deletes the
// component's folder in kiln/vendor/ after a confirmation. `useUpscalerFiles` holds that flow for a game (the Graphics
// card's offer and the list share it). kiln_manage::vendor does the work (commands/vendor.rs); docs/kiln/graphics.md
// "Upscaler files".
import { useQuery, useQueryClient, type UseQueryResult } from "@tanstack/react-query";
import { useState, type ReactNode } from "react";
import { create } from "zustand";
import * as api from "../api/commands";
import type { GameId, KilnError, Task } from "../api/commands";
import { vendorDownload, vendorLicense, vendorRemove, vendorStatus, vk, type VendorComponent, type VendorDownloadReport, type VendorStatus } from "../api/vendor";
import { t as tr, useT } from "../i18n";
import { Banner, Button, EmptyState, KeyValue, Modal, ProgressBar, Skeleton, StatusPill, errorText, fmtBytes, toast, useTasks, waitTask, type Tone } from "../ui";

/** What a row shows. `failed` is a download that failed (its task's reason) or files that are missing or changed. */
export type UpscalerRowState = "notDownloaded" | "downloading" | "downloaded" | "updateAvailable" | "unavailable" | "failed";
export type UpscalerAction = "download" | "continue" | "update" | "remove" | "tryAgain" | "downloadAgain" | "cancel";

export interface UpscalerRow {
  state: UpscalerRowState;
  /** The line under the name. */
  detail: string;
  /** For `failed`: why. */
  reason?: string;
  actions: UpscalerAction[];
  /** For `downloading`: 0..1, or null while unknown. */
  progress?: number | null;
}

const running = (task: Task | undefined) => !!task && (task.state === "running" || task.state === "queued");

/** The row of a component from its status and the last download task of it in this window (if any). */
export function upscalerRow(c: VendorComponent, task?: Task): UpscalerRow {
  const host = c.hosts.join(", ");
  if (running(task)) return { state: "downloading", detail: task!.message ?? tr("settings.upscalers.starting"), progress: task!.progress ?? null, actions: ["cancel"] };
  if (c.state === "unavailable") return { state: "unavailable", detail: c.reason ?? tr("settings.upscalers.unavailable"), actions: [] };
  const installedLine = c.installed ? tr("settings.upscalers.installed", { version: c.installed.version, size: fmtBytes(c.installed.sizeOnDisk) }) : "";
  if (task?.state === "failed" && c.state !== "downloaded") {
    return {
      state: "failed", detail: c.installed ? installedLine : tr("settings.upscalers.size", { size: fmtBytes(c.size), host }),
      reason: task.error?.message ?? task.message ?? tr("settings.upscalers.failedUnknown"), actions: c.installed ? ["tryAgain", "remove"] : ["tryAgain"],
    };
  }
  if (c.state === "damaged") return { state: "failed", detail: installedLine, reason: c.reason ?? tr("settings.upscalers.damaged"), actions: ["downloadAgain", "remove"] };
  if (c.state === "downloaded") return { state: "downloaded", detail: installedLine, actions: ["remove"] };
  if (c.state === "updateAvailable") {
    return { state: "updateAvailable", detail: tr("settings.upscalers.update", { installed: c.installed?.version ?? "", version: c.version, size: fmtBytes(c.size), host }), actions: ["update", "remove"] };
  }
  const detail = tr("settings.upscalers.size", { size: fmtBytes(c.size), host });
  if (c.partial > 0) return { state: "notDownloaded", detail: `${detail} ${tr("settings.upscalers.partial", { done: fmtBytes(c.partial) })}`, actions: ["continue"] };
  return { state: "notDownloaded", detail, actions: ["download"] };
}

const TONE: Record<UpscalerRowState, Tone> = { notDownloaded: "idle", downloading: "ember", downloaded: "ok", updateAvailable: "info", unavailable: "idle", failed: "err" };

/** The download task of each row in this window, by game and component (the task store keeps the tasks; the Deck's
 *  rows read the same). */
export const useRowTasks = create<{ ids: Record<string, string>; set: (key: string, id: string) => void }>((set) => ({
  ids: {},
  set: (key, id) => set((s) => ({ ids: { ...s.ids, [key]: id } })),
}));
export const rowKey = (gameId: GameId, id: string) => `${gameId}/${id}`;

/** The download task of a component in this window, if any. */
export function useRowTask(gameId: GameId, id: string | undefined): Task | undefined {
  const tid = useRowTasks((s) => (id ? s.ids[rowKey(gameId, id)] : undefined));
  return useTasks((s) => (tid ? s.tasks[tid] : undefined));
}

export interface UpscalerFiles {
  status: UseQueryResult<VendorStatus>;
  /** The row of a component now. */
  row: (c: VendorComponent) => UpscalerRow;
  task: (c: VendorComponent) => Task | undefined;
  /** Download (the licence dialog first when its text is not agreed to yet), update, go on, try again. */
  download: (c: VendorComponent) => void;
  /** Asks, then removes. */
  remove: (c: VendorComponent) => void;
  cancel: (c: VendorComponent) => void;
  /** The licence dialog and the removal's question as the desktop shows them: render once. The Deck shows its own from
   *  the fields below. */
  dialogs: ReactNode;
  /** The component whose licence the player is asked to agree to (its first download), or null. */
  asking: VendorComponent | null;
  /** "I agree, download": the hash of the licence text shown. */
  agree: (sha256: string) => void;
  cancelAsk: () => void;
  /** The component the player is asked to remove, or null. */
  removing: VendorComponent | null;
  confirmRemove: () => void;
  cancelRemove: () => void;
}

/** The vendor files of a client: their status, the download flow (licence first, progress, Cancel), Remove. */
export function useUpscalerFiles(gameId: GameId): UpscalerFiles {
  const t = useT();
  const qc = useQueryClient();
  const status = useQuery({ queryKey: vk.status(gameId), queryFn: () => vendorStatus(gameId) });
  const ids = useRowTasks((s) => s.ids);
  const setTask = useRowTasks((s) => s.set);
  const tasks = useTasks((s) => s.tasks);
  const [ask, setAsk] = useState<VendorComponent | null>(null);
  const [drop, setDrop] = useState<VendorComponent | null>(null);
  const refresh = () => qc.invalidateQueries({ queryKey: vk.status(gameId) });
  const taskOf = (c: VendorComponent) => (ids[rowKey(gameId, c.id)] ? tasks[ids[rowKey(gameId, c.id)]] : undefined);

  const start = async (c: VendorComponent, licenseSha256: string | null) => {
    let id: string;
    try {
      id = await vendorDownload(gameId, c.id, licenseSha256);
    } catch (e) {
      // the agreement was lost (another data folder, a deleted file): the dialog again
      if ((e as KilnError).code === "consent_required") setAsk(c);
      else toast.error(e, t("settings.upscalers.failedTitle", { name: c.name }));
      void refresh();
      return;
    }
    setTask(rowKey(gameId, c.id), id);
    void refresh();
    try {
      const r = await waitTask<VendorDownloadReport>(id);
      toast.ok(t("settings.upscalers.done", { name: c.name }), t("settings.upscalers.doneDetail", { folder: r.folder }));
      for (const w of r.warnings) toast.error({ code: "io", message: w });
    } catch {
      // the row shows the reason (failed) or goes back to its state (canceled)
    } finally {
      void refresh();
    }
  };
  const download = (c: VendorComponent) => {
    if (taskOf(c)?.state === "running" || taskOf(c)?.state === "queued") return;
    if (c.license.agreed) void start(c, null);
    else setAsk(c);
  };
  const agree = (sha: string) => { const c = ask; setAsk(null); if (c) void start(c, sha); };
  const confirmRemove = async () => {
    const c = drop;
    setDrop(null);
    if (!c) return;
    try {
      const r = await vendorRemove(gameId, c.id);
      toast.ok(t("settings.upscalers.removed", { name: c.name }), t("settings.upscalers.removedDetail", { size: fmtBytes(r.freed) }));
      for (const k of r.kept) toast.error({ code: "io", message: k });
    } catch (e) {
      toast.error(e, t("settings.upscalers.removeFailed", { name: c.name }));
    } finally {
      void refresh();
    }
  };
  const dialogs = (
    <>
      {ask && <LicenseDialog c={ask} onClose={() => setAsk(null)} onAgree={agree} />}
      <Modal open={!!drop} onClose={() => setDrop(null)} title={drop ? t("settings.upscalers.removeTitle", { name: drop.name }) : ""} width={480}
        footer={<>
          <Button onClick={() => setDrop(null)}>{t("common.cancel")}</Button>
          <Button variant="danger" onClick={() => void confirmRemove()}>{t("common.remove")}</Button>
        </>}>
        {drop && removeText(drop)}
      </Modal>
    </>
  );
  return {
    status, row: (c) => upscalerRow(c, taskOf(c)), task: taskOf, download, remove: setDrop,
    cancel: (c) => { const task = taskOf(c); if (task) api.taskCancel(task.id).catch((e) => toast.error(e)); },
    dialogs, asking: ask, agree, cancelAsk: () => setAsk(null), removing: drop, confirmRemove: () => void confirmRemove(), cancelRemove: () => setDrop(null),
  };
}

/** What Remove deletes, one sentence. */
export const removeText = (c: VendorComponent) =>
  tr("settings.upscalers.removeText", { folder: c.installed?.folder ?? `kiln/vendor/${c.vendor}/${c.version}`, size: fmtBytes(c.installed?.sizeOnDisk ?? 0) });

/** The licence dialog's facts: what, size, from, to (the desktop's and the Deck's). */
export function licenseFacts(c: VendorComponent): [string, string][] {
  const host = c.hosts.join(", ");
  return [
    [tr("settings.upscalers.dialog.what"), c.enables],
    [tr("settings.upscalers.dialog.size"), tr("settings.upscalers.dialog.files", { count: c.files.length, size: fmtBytes(c.size) })],
    [tr("settings.upscalers.dialog.from"), tr("settings.upscalers.dialog.fromText", { host, vendor: c.vendorName, version: c.version })],
    [tr("settings.upscalers.dialog.to"), tr("settings.upscalers.dialog.toText", { folder: `kiln/vendor/${c.vendor}/${c.version}` })],
  ];
}

/** Settings, Graphics: the upscaler files' list (`#upscalers`), under the choice. */
export function UpscalerFilesList({ files }: { files: UpscalerFiles }) {
  const t = useT();
  const st = files.status;
  return (
    <section id="upscalers" className="k-stack" style={{ gap: 6 }} aria-label={t("settings.upscalers.title")}>
      <span className="k-panel-title">{t("settings.upscalers.title")}</span>
      <span className="k-hint">{t("settings.upscalers.sub")}</span>
      <p className="k-hint" data-testid="upscalers-purpose">{t("settings.upscalers.purpose")}</p>
      {st.isLoading ? <Skeleton lines={2} /> : st.error ? <EmptyState title={t("settings.upscalers.unreadable")} hint={errorText(st.error)} /> : (
        <ul className="k-upscalers" aria-label={t("settings.upscalers.title")}>
          {st.data!.components.map((c) => (
            <UpscalerRowView key={c.id} c={c} row={files.row(c)} onDownload={() => files.download(c)} onRemove={() => files.remove(c)} onCancel={() => files.cancel(c)} />
          ))}
        </ul>
      )}
    </section>
  );
}

export function UpscalerRowView({ c, row, onDownload, onRemove, onCancel }: { c: VendorComponent; row: UpscalerRow; onDownload: () => void; onRemove: () => void; onCancel: () => void }) {
  const t = useT();
  const button = (a: UpscalerAction) => {
    switch (a) {
      case "download": case "continue": case "update": case "tryAgain": case "downloadAgain":
        return <Button key={a} size="s" variant={a === "download" || a === "update" ? "primary" : "secondary"} aria-label={t(`settings.upscalers.actionLabel.${a}`, { name: c.name })} onClick={onDownload}>{t(`settings.upscalers.action.${a}`)}</Button>;
      case "remove":
        return <Button key={a} size="s" variant="ghost" aria-label={t("settings.upscalers.actionLabel.remove", { name: c.name })} onClick={onRemove}>{t("common.remove")}</Button>;
      case "cancel":
        return <Button key={a} size="s" variant="ghost" aria-label={t("settings.upscalers.actionLabel.cancel", { name: c.name })} onClick={onCancel}>{t("common.cancel")}</Button>;
    }
  };
  return (
    <li className="k-upscaler" aria-label={c.name} data-state={row.state}>
      <div className="k-upscaler-head">
        <span className="k-upscaler-name">{c.name} <span className="k-mono k-muted">{c.version}</span></span>
        <StatusPill tone={TONE[row.state]}>{t(`settings.upscalers.state.${row.state}`)}</StatusPill>
        <span className="k-upscaler-actions">{row.actions.map(button)}</span>
      </div>
      <div className="k-upscaler-enables">{c.enables}</div>
      {row.state === "downloading" && (
        <div className="k-upscaler-progress">
          <ProgressBar value={row.progress ?? 0} />
          {row.progress !== null && row.progress !== undefined && <span className="k-task-pct">{Math.round(row.progress * 100)}%</span>}
        </div>
      )}
      <div className="k-upscaler-detail" data-testid="upscaler-detail">{row.detail}</div>
      {row.reason && <Banner tone="err"><span data-testid="upscaler-reason">{row.reason}</span></Banner>}
    </li>
  );
}

/** The licence dialog of a vendor's first download: what it is, the size, the host, where it goes, the licence in
 *  full, what the host sees; "I agree, download" passes the hash of the text shown. */
function LicenseDialog({ c, onClose, onAgree }: { c: VendorComponent; onClose: () => void; onAgree: (sha256: string) => void }) {
  const t = useT();
  const lic = useQuery({ queryKey: vk.license(c.id), queryFn: () => vendorLicense(c.id) });
  const host = c.hosts.join(", ");
  return (
    <Modal open onClose={onClose} title={t("settings.upscalers.dialog.title", { name: c.name })} width={640}
      footer={<>
        <Button onClick={onClose}>{t("common.cancel")}</Button>
        <Button variant="primary" disabled={!lic.data} onClick={() => lic.data && onAgree(lic.data.sha256)}>{t("settings.upscalers.dialog.agree")}</Button>
      </>}>
      <div className="k-stack" style={{ gap: 10 }}>
        <KeyValue rows={licenseFacts(c)} />
        <div className="k-upscaler-license-head">
          <span className="k-panel-title">{c.license.name}</span>
          <button type="button" className="k-link" onClick={() => api.appOpenUrl(c.license.from).catch((e) => toast.error(e))}>{t("settings.upscalers.dialog.licenseFrom")}</button>
        </div>
        <span className="k-hint">{t("settings.upscalers.dialog.licenseLead", { vendor: c.vendorName, license: c.license.name })}</span>
        {lic.isLoading ? <Skeleton lines={4} /> : lic.error ? <EmptyState title={t("settings.upscalers.dialog.unreadable")} hint={errorText(lic.error)} /> : (
          <pre className="k-upscaler-license" tabIndex={0} aria-label={c.license.name} data-testid="upscaler-license">{lic.data!.text}</pre>
        )}
        <span className="k-hint" data-testid="upscaler-privacy">{t("settings.upscalers.dialog.privacy", { host })}</span>
      </div>
    </Modal>
  );
}
