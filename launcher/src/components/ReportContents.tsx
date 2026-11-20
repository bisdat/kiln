// "What's in this report" (docs/kiln/launcher/support-report.md): every file of a support report grouped as Kiln's logs,
// other mods' logs, settings and folder listings, each with one plain line on what it holds and how often Kiln replaced
// something personal in it (kiln_manage::report::contents), a View of summary.txt, and checkboxes that leave other
// mods' logs out (Kiln's own files always stay: support needs them). Desktop "Save report..." opens it as a dialog
// first; "Send to Kiln support" shows it on its check step; the Deck's send flow has its own rows (DeckSend.tsx).
import { useState } from "react";
import { keepPreviousData, useQuery } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { LogTarget, ReportContents, ReportContentsFile, ReportFileGroup } from "../api/commands";
import { t as tt, useT } from "../i18n";
import { Banner, Button, Modal, Skeleton, fmtBytes } from "../ui";
import "./report-contents.css";

export const GROUPS: ReportFileGroup[] = ["kiln", "mods", "settings", "listings"];
const ABOUT = new Set(["summary", "findings", "kiln-log", "proxy-log", "eml-log", "tick-stats", "greeneye-bridge", "crash", "launcher-log", "game-start",
  "kiln-other", "state", "profile", "chain", "kiln-json", "receipt", "game-folder", "kiln-folder", "mod-folders", "listing", "xhl-core", "greeneye",
  "building-tools", "shroudlink", "auto-loot", "optiscaler-settings", "optiscaler-log", "mod-log"]);
const TOKENS = ["home", "user", "host", "steam-name", "steamid", "player", "ip", "mac", "email", "redacted"];
const NEVER = new Set(["saves", "settings", "admin", "mods", "steam"]);

/** The plain line for what a file holds. */
export const aboutText = (f: ReportContentsFile) => tt(`reportContents.about.${ABOUT.has(f.about) ? f.about : f.group === "kiln" ? "kiln-other" : "mod-log"}`);

/** "Replaced: your home folder (169 times), player names (3 times)"; null: nothing replaced. */
export function replacedText(replaced: Record<string, number>): string | null {
  const items = TOKENS.filter((k) => (replaced[k] ?? 0) > 0)
    .map((k) => tt("reportContents.item", { what: tt(`reportContents.token.${k}`), times: tt("reportContents.times", { count: replaced[k] }) }));
  return items.length ? tt("reportContents.replaced", { list: items.join(tt("reportContents.sep")) }) : null;
}

/** Every replacement in the files that go, added up. */
export function replacedInAll(c: ReportContents): Record<string, number> {
  const sum: Record<string, number> = {};
  for (const f of c.files) if (!f.excluded) for (const [k, n] of Object.entries(f.replaced)) sum[k] = (sum[k] ?? 0) + n;
  return sum;
}

/** "13 files, 112 KB before compression · 1 file left out" */
export function totalText(c: ReportContents): string {
  const n = c.files.filter((f) => !f.excluded).length;
  const out = c.files.length - n;
  return [tt("reportContents.total", { count: n, size: fmtBytes(c.size) }), out ? tt("reportContents.leftOutCount", { count: out }) : null].filter(Boolean).join(" · ");
}

/** "1 file left out", or null. */
export function leftOutText(c: ReportContents): string | null {
  const out = c.files.filter((f) => f.excluded).length;
  return out ? tt("reportContents.leftOutCount", { count: out }) : null;
}

/** The never-collected item in the launcher's language (English from the report when the launcher has no words). */
export const neverText = (n: { id: string; text: string }) => (NEVER.has(n.id) ? tt(`reportContents.never.${n.id}`) : n.text);

/** Toggles one file, or every other mod's file (`null`), in a list of left-out paths. */
export function toggled(exclude: string[], c: ReportContents, path: string | null): string[] {
  if (path !== null) return exclude.includes(path) ? exclude.filter((p) => p !== path) : [...exclude, path];
  const all = c.files.filter((f) => f.excludable).map((f) => f.path);
  return all.every((p) => exclude.includes(p)) ? [] : all;
}

/** The grouped files, what was replaced, the checkboxes and what is never collected. */
export function ReportContentsView({ contents, exclude, onExclude, disabled }: {
  contents: ReportContents; exclude: string[]; onExclude: (next: string[]) => void; disabled?: boolean;
}) {
  const t = useT();
  const mods = contents.files.filter((f) => f.excludable);
  const allOut = mods.length > 0 && mods.every((f) => exclude.includes(f.path));
  return (
    <div className="krc" data-testid="krc">
      {GROUPS.map((g) => {
        const files = contents.files.filter((f) => f.group === g);
        if (!files.length) return null;
        return (
          <section key={g} className="krc-group" data-testid={`krc-group-${g}`}>
            <div className="krc-group-head">
              <span className="k-label">{t(`reportContents.group.${g}`)}</span>
              {g === "mods" && mods.length > 1 && (
                <label className="krc-leave">
                  <input type="checkbox" checked={allOut} disabled={disabled} data-testid="krc-leave-all" onChange={() => onExclude(toggled(exclude, contents, null))} />
                  {t("reportContents.leaveOutAll")}
                </label>
              )}
            </div>
            <span className="k-hint">{t(`reportContents.groupHint.${g}`)}</span>
            <ul className="krc-files">
              {files.map((f) => {
                const out = exclude.includes(f.path);
                const replaced = replacedText(f.replaced);
                return (
                  <li key={f.path} className={"krc-file" + (out ? " out" : "")} data-testid="krc-file" data-path={f.path}>
                    <span className="krc-file-main">
                      <span className="krc-about">{aboutText(f)}</span>
                      <span className="k-mono krc-path">{f.path} · {fmtBytes(f.size)}</span>
                      <span className="k-hint">{out ? t("reportContents.leftOutHint") : replaced ?? t("reportContents.nothing")}</span>
                    </span>
                    {f.excludable && (
                      <label className="krc-leave">
                        <input type="checkbox" checked={out} disabled={disabled} aria-label={`${t("reportContents.leaveOut")}: ${f.path}`} onChange={() => onExclude(toggled(exclude, contents, f.path))} />
                        {t("reportContents.leaveOut")}
                      </label>
                    )}
                  </li>
                );
              })}
            </ul>
          </section>
        );
      })}
      <section className="krc-group" data-testid="krc-never">
        <span className="k-label">{t("reportContents.never.title")}</span>
        <ul className="krc-never">{contents.never.map((n) => <li key={n.id}>{neverText(n)}</li>)}</ul>
      </section>
    </div>
  );
}

/** summary.txt as it is in the zip. */
export function SummaryText({ text }: { text: string }) {
  return <pre className="krc-summary k-mono" data-testid="krc-summary" tabIndex={0}>{text}</pre>;
}

/** "Save report...": what the report holds first, then Save (the caller asks where) or Cancel. */
export function ReportContentsDialog({ target, onSave, onClose }: { target: LogTarget | null; onSave: (exclude: string[]) => void; onClose: () => void }) {
  const t = useT();
  const [exclude, setExclude] = useState<string[]>([]);
  const [view, setView] = useState(false);
  const q = useQuery({ queryKey: ["report", "preview", target, exclude], queryFn: () => api.reportPreview(target, exclude), placeholderData: keepPreviousData, staleTime: 30_000, retry: false });
  const c = q.data;
  if (view && c) {
    return (
      <Modal open onClose={() => setView(false)} title={t("reportContents.viewTitle")} width={760}
        footer={<Button onClick={() => setView(false)}>{t("common.back")}</Button>}>
        <SummaryText text={c.summary} />
      </Modal>
    );
  }
  const replaced = c ? replacedText(replacedInAll(c)) : null;
  return (
    <Modal open onClose={onClose} title={t("reportContents.title")} width={720}
      footer={<>
        <Button onClick={onClose}>{t("common.cancel")}</Button>
        <Button variant="primary" data-testid="krc-save" disabled={!c && !q.error} onClick={() => onSave(exclude)}>{t("reportContents.save")}</Button>
      </>}>
      <div className="k-stack" style={{ gap: 12 }} data-testid="krc-dialog">
        <p className="kc-p">{t("reportContents.intro")}</p>
        {q.error ? <Banner tone="err" title={t("reportContents.failed")}>{(q.error as { message?: string }).message ?? ""}</Banner> : !c ? <Skeleton lines={6} /> : <>
          <div className="krc-total">
            <span data-testid="krc-total">{totalText(c)}</span>
            <Button size="s" variant="ghost" data-testid="krc-view" onClick={() => setView(true)}>{t("reportContents.view")}</Button>
          </div>
          {replaced && <span className="k-hint" data-testid="krc-replaced-all">{replaced}</span>}
          <ReportContentsView contents={c} exclude={exclude} onExclude={setExclude} />
        </>}
      </div>
    </Modal>
  );
}
