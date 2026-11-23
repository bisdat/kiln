import { expect, test } from "bun:test";
import type { ReportNote } from "../src/api/commands";
import { reportDestination, reportFileName } from "../src/ui/reportSave";

function deps(choose: () => Promise<string | null>) {
  const notes: [string, string | undefined][] = [];
  return { choose, notes, note: async (note: ReportNote, detail?: string) => { notes.push([note, detail]); return null; } };
}

test("a report keeps the chosen Linux or Windows path and logs it", async () => {
  for (const path of ["/home/player/report.zip", "C:\\Users\\Player\\Downloads\\report.zip"]) {
    const d = deps(async () => path);
    expect(await reportDestination("dialog", d)).toEqual({ path, destination: "chosen" });
    expect(d.notes).toEqual([["dialogChosen", path]]);
  }
});

test("cancelling the report dialog never triggers a fallback save", async () => {
  const d = deps(async () => null);
  expect(await reportDestination("dialog", d)).toBeNull();
  expect(d.notes).toEqual([["dialogCancelled", undefined]]);
});

test("a report dialog error is logged with its OS error and selects the owned-folder fallback", async () => {
  const d = deps(async () => { throw new Error("GTK save: Permission denied (os error 13)"); });
  expect(await reportDestination("dialog", d)).toEqual({ path: reportFileName(), destination: "automatic" });
  expect(d.notes).toEqual([["dialogFailed", "GTK save: Permission denied (os error 13)"]]);
});

test("Save to Downloads logs the action and never opens the native dialog", async () => {
  const d = deps(async () => { throw new Error("the dialog must not open"); });
  expect(await reportDestination("downloads", d)).toEqual({ path: reportFileName(), destination: "downloads" });
  expect(d.notes).toEqual([["saveToDownloads", undefined]]);
});

test("the default report name is local and is safe as a bare filename on Linux and Windows", () => {
  expect(reportFileName(new Date(2026, 0, 2, 23, 59))).toBe("kiln-report-2026-01-02.zip");
});
