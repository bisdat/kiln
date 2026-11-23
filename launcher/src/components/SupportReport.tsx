// "Support report": one zip with what a maintainer needs when mods don't load (report_save), and a few lines to paste
// into a post (report_summary). "Save report..." shows what is in it first ("What's in this report", report_preview),
// where other mods' logs can be left out. Both are redacted by kiln_manage::report: names and paths never leave the machine.
// Used by the Logs page (a game or a local server; at the top of every tab), where Play's problem banners point.
// docs/kiln/launcher/support-report.md
import { useState } from "react";
import * as api from "../api/commands";
import type { LogTarget } from "../api/commands";
import { useAppInfo } from "../api/hooks";
import { useT } from "../i18n";
import { Button, errorText, fmtBytes, toast } from "../ui";
import { SendToSupport } from "../community/parts";
import { ReportContentsDialog } from "./ReportContents";
import { reportDestination } from "../ui/reportSave";
export { reportFileName } from "../ui/reportSave";

/** The block: one sentence, "Save report...", "Copy summary". `target` null: the system and the launcher only. */
export function SupportReport({ target, compact }: { target: LogTarget | null; compact?: boolean }) {
  const t = useT();
  const deck = useAppInfo().data?.deck;
  const [busy, setBusy] = useState<"save" | "copy" | null>(null);
  const [contents, setContents] = useState(false);
  const save = async (exclude: string[], mode: "dialog" | "downloads" = "dialog") => {
    setContents(false);
    setBusy("save");
    try {
      const dest = await reportDestination(mode);
      if (!dest) return;
      const r = await api.reportSave(target, dest.path, exclude, dest.destination);
      const problems = r.findings.filter((f) => f.level === "problem").length;
      const folder = r.path.replace(/([\\/])[^\\/]*$/, "$1");
      toast.ok(t("supportReport.saved"), [
        t("supportReport.savedPath", { path: r.path, size: fmtBytes(r.size) }),
        problems ? t("supportReport.problems", { count: problems }) : null,
        t("supportReport.attach"),
      ].filter(Boolean).join(" "),
        { actions: folder && folder !== r.path ? [{ label: t("supportReport.openFolder"), onClick: () => { api.appOpenPath(folder).catch((e) => toast.error(e)); } }] : [] });
    } catch (e) {
      void api.reportNote("failed", errorText(e));
      toast.error(e, t("supportReport.notSaved"));
    } finally {
      setBusy(null);
    }
  };
  const copy = async () => {
    setBusy("copy");
    try {
      const text = await api.reportSummary(target);
      if (!navigator.clipboard) throw new Error(t("supportReport.noClipboard"));
      await navigator.clipboard.writeText(text);
      toast.ok(t("supportReport.copied"), t("supportReport.copiedDetail"));
    } catch (e) {
      toast.error(e, t("supportReport.copyFailed"));
    } finally {
      setBusy(null);
    }
  };
  return (
    <div className="k-list-item" data-testid="support-report" style={{ flexWrap: "wrap", rowGap: 6, padding: compact ? "6px 0" : "10px 0" }}>
      <span className="k-word-sm">{t("supportReport.title")}</span>
      <span className="k-hint" style={{ flex: "1 1 260px" }}>
        {t("supportReport.hint")}
      </span>
      <SendToSupport from="logs" target={target} variant="primary" />
      <Button size="s" loading={busy === "save"} disabled={!!busy} onClick={() => { void api.reportNote("opened"); setContents(true); }}>{t("supportReport.save")}</Button>
      {(deck?.detected || deck?.gamescope) && <Button size="s" disabled={!!busy} onClick={() => void save([], "downloads")}>{t("supportReport.saveDownloads")}</Button>}
      <Button size="s" variant="ghost" loading={busy === "copy"} disabled={!!busy} onClick={copy}>{t("supportReport.copy")}</Button>
      {contents && <ReportContentsDialog target={target} onSave={(x) => void save(x)} onClose={() => setContents(false)} />}
    </div>
  );
}
