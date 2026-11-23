// The report's native Save dialog and its dialog-free destinations. A cancelled dialog still saves nothing.
import * as api from "../api/commands";
import { t } from "../i18n";
import { saveDialog } from "./dialogFolder";
import { errorText } from "./format";

/** `kiln-report-2026-11-10.zip` (the crate's report::file_name, in local time). */
export function reportFileName(d = new Date()): string {
  const p = (n: number) => String(n).padStart(2, "0");
  return `kiln-report-${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}.zip`;
}

async function choosePath(): Promise<string | null> {
  if (!api.isTauri()) return reportFileName();
  return saveDialog({ kind: "supportReport", title: t("supportReport.dialogTitle"), fileName: reportFileName(), filters: [{ name: "Zip", extensions: ["zip"] }] });
}

/** The save command resolves automatic/Downloads destinations with native paths and reports where it really saved. */
export async function reportDestination(mode: "dialog" | "downloads", deps = { choose: choosePath, note: api.reportNote }): Promise<{ path: string; destination: api.ReportSaveDestination } | null> {
  if (mode === "downloads") {
    await deps.note("saveToDownloads");
    return { path: reportFileName(), destination: "downloads" };
  }
  let path: string | null;
  try {
    path = await deps.choose();
  } catch (e) {
    await deps.note("dialogFailed", errorText(e));
    return { path: reportFileName(), destination: "automatic" };
  }
  if (!path) {
    await deps.note("dialogCancelled");
    return null;
  }
  await deps.note("dialogChosen", path);
  return { path, destination: "chosen" };
}
