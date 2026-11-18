// Settings, Upscalers (dev.29, behind the preview "upscalers": api/vendor.ts `upscalersPreview`): the vendor upscaler
// files Kiln downloads only when the player asks (AMD FSR, Intel XeSS), one row per component of the launcher's
// manifest, right under the Graphics card, for a client only. A first download of a vendor's licence text opens the
// licence dialog (what, size, host, where it goes, the text in full, the privacy line; "I agree, download"); later
// ones start from the row, which says the size and the host. Downloads run as tasks (progress, Cancel); a failed one
// shows its reason with Try again. Remove deletes the component's folder in kiln/vendor/ after a confirmation.
// kiln_manage::vendor does the work (commands/vendor.rs); docs/kiln/graphics.md "Upscaler files".
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import { create } from "zustand";
import * as api from "../api/commands";
import type { GameId, KilnError, Task } from "../api/commands";
import { useAppInfo, useGame, useGameStatus } from "../api/hooks";
import { upscalersPreview, vendorDownload, vendorLicense, vendorRemove, vendorStatus, vk, type VendorComponent, type VendorDownloadReport } from "../api/vendor";
import { t as tr, useT } from "../i18n";
import { Badge, Banner, Button, Card, EmptyState, KeyValue, Modal, ProgressBar, Skeleton, StatusPill, errorText, fmtBytes, toast, useTasks, waitTask, type Tone } from "../ui";

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

/** The download task of each row in this window, by game and component (the task store keeps the tasks). */
const useRowTasks = create<{ ids: Record<string, string>; set: (key: string, id: string) => void }>((set) => ({
  ids: {},
  set: (key, id) => set((s) => ({ ids: { ...s.ids, [key]: id } })),
}));
const rowKey = (gameId: GameId, id: string) => `${gameId}/${id}`;

export function UpscalersCard() {
  const info = useAppInfo();
  const { game } = useGame();
  // a dedicated server draws no frames; the preview is off unless the player turned it on
  if (!game || game.kind !== "client" || !upscalersPreview(info.data)) return null;
  return <Upscalers gameId={game.id} />;
}

function Upscalers({ gameId }: { gameId: GameId }) {
  const t = useT();
  const qc = useQueryClient();
  const gs = useGameStatus(gameId);
  const st = useQuery({ queryKey: vk.status(gameId), queryFn: () => vendorStatus(gameId) });
  const ids = useRowTasks((s) => s.ids);
  const setTask = useRowTasks((s) => s.set);
  const tasks = useTasks((s) => s.tasks);
  const [ask, setAsk] = useState<VendorComponent | null>(null);
  const [drop, setDrop] = useState<VendorComponent | null>(null);
  const refresh = () => qc.invalidateQueries({ queryKey: vk.status(gameId) });

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
  const download = (c: VendorComponent) => (c.license.agreed ? void start(c, null) : setAsk(c));

  const installed = gs.data?.runtime.installed;
  return (
    <Card id="upscalers" title={t("settings.upscalers.title")} sub={t("settings.upscalers.sub")} aria-label={t("settings.upscalers.title")}
      actions={<Badge tone="ember">{t("settings.upscalers.preview")}</Badge>}>
      <p className="k-hint" data-testid="upscalers-purpose">{t("settings.upscalers.purpose")}</p>
      {gs.data && !installed ? <EmptyState title={t("settings.kilnJson.notInstalled")} hint={t("settings.kilnJson.notInstalledHint")} /> :
        st.isLoading ? <Skeleton lines={2} /> : st.error ? <EmptyState title={t("settings.upscalers.unreadable")} hint={errorText(st.error)} /> : (
          <ul className="k-upscalers" aria-label={t("settings.upscalers.title")}>
            {st.data!.components.map((c) => {
              const task = ids[rowKey(gameId, c.id)] ? tasks[ids[rowKey(gameId, c.id)]] : undefined;
              const row = upscalerRow(c, task);
              return <UpscalerRowView key={c.id} c={c} row={row} task={task} onDownload={() => download(c)} onRemove={() => setDrop(c)} />;
            })}
          </ul>
        )}
      {ask && <LicenseDialog c={ask} onClose={() => setAsk(null)} onAgree={(sha) => { const c = ask; setAsk(null); void start(c, sha); }} />}
      <Modal open={!!drop} onClose={() => setDrop(null)} title={drop ? t("settings.upscalers.removeTitle", { name: drop.name }) : ""} width={480}
        footer={<>
          <Button onClick={() => setDrop(null)}>{t("common.cancel")}</Button>
          <Button variant="danger" onClick={async () => {
            const c = drop!;
            setDrop(null);
            try {
              const r = await vendorRemove(gameId, c.id);
              toast.ok(t("settings.upscalers.removed", { name: c.name }), t("settings.upscalers.removedDetail", { size: fmtBytes(r.freed) }));
              for (const k of r.kept) toast.error({ code: "io", message: k });
            } catch (e) {
              toast.error(e, t("settings.upscalers.removeFailed", { name: c.name }));
            } finally {
              void refresh();
            }
          }}>{t("common.remove")}</Button>
        </>}>
        {drop && t("settings.upscalers.removeText", { folder: drop.installed?.folder ?? `kiln/vendor/${drop.vendor}/${drop.version}`, size: fmtBytes(drop.installed?.sizeOnDisk ?? 0) })}
      </Modal>
    </Card>
  );
}

function UpscalerRowView({ c, row, task, onDownload, onRemove }: { c: VendorComponent; row: UpscalerRow; task?: Task; onDownload: () => void; onRemove: () => void }) {
  const t = useT();
  const button = (a: UpscalerAction) => {
    switch (a) {
      case "download": case "continue": case "update": case "tryAgain": case "downloadAgain":
        return <Button key={a} size="s" variant={a === "download" || a === "update" ? "primary" : "secondary"} aria-label={t(`settings.upscalers.actionLabel.${a}`, { name: c.name })} onClick={onDownload}>{t(`settings.upscalers.action.${a}`)}</Button>;
      case "remove":
        return <Button key={a} size="s" variant="ghost" aria-label={t("settings.upscalers.actionLabel.remove", { name: c.name })} onClick={onRemove}>{t("common.remove")}</Button>;
      case "cancel":
        return <Button key={a} size="s" variant="ghost" aria-label={t("settings.upscalers.actionLabel.cancel", { name: c.name })} onClick={() => task && api.taskCancel(task.id).catch((e) => toast.error(e))}>{t("common.cancel")}</Button>;
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
  const folder = `kiln/vendor/${c.vendor}/${c.version}`;
  return (
    <Modal open onClose={onClose} title={t("settings.upscalers.dialog.title", { name: c.name })} width={640}
      footer={<>
        <Button onClick={onClose}>{t("common.cancel")}</Button>
        <Button variant="primary" disabled={!lic.data} onClick={() => lic.data && onAgree(lic.data.sha256)}>{t("settings.upscalers.dialog.agree")}</Button>
      </>}>
      <div className="k-stack" style={{ gap: 10 }}>
        <KeyValue rows={[
          [t("settings.upscalers.dialog.what"), c.enables],
          [t("settings.upscalers.dialog.size"), t("settings.upscalers.dialog.files", { count: c.files.length, size: fmtBytes(c.size) })],
          [t("settings.upscalers.dialog.from"), t("settings.upscalers.dialog.fromText", { host, vendor: c.vendorName, version: c.version })],
          [t("settings.upscalers.dialog.to"), t("settings.upscalers.dialog.toText", { folder })],
        ]} />
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
