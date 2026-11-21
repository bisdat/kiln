// Raw JSON: any layer file as text (a plain editor), new / rename / delete content/<name>.json, and the lint of the
// file shown under it.
import { useEffect, useState } from "react";
import type { LintDiagnostic } from "../../api/commands";
import { useT } from "../../i18n";
import { Button, ConfirmDanger, TextArea, TextInput, toast } from "../../ui";
import { useDataCtx } from "./Picker";
import { useDrafts, useLint, useWriteLayer } from "./store";

const NAME = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;
/** "stacks" or "stacks.json" -> content/stacks.json; null when not a layer file name. */
export function layerPath(name: string): string | null {
  let n = name.trim().replace(/^content\//, "");
  if (!/\.jsonc?$/i.test(n)) n += ".json";
  return NAME.test(n) && n.length <= 96 ? "content/" + n : null;
}
const EMPTY = '{\n  "format": 1\n}\n';

export default function RawEditor({ texts }: { texts: Record<string, string> | undefined }) {
  const t = useT();
  const { packId } = useDataCtx();
  const files = Object.keys(texts ?? {}).sort();
  const [sel, setSel] = useState<string | null>(null);
  const cur = sel && texts && sel in texts ? sel : files[0] ?? null;
  const key = packId + ":raw:" + (cur ?? "");
  const draft = useDrafts((s) => s.by[key]) as { value: string; dirty: boolean } | undefined;
  const setDraft = useDrafts((s) => s.set);
  const text = draft?.dirty ? draft.value : cur ? texts?.[cur] ?? "" : "";
  const { write, remove } = useWriteLayer(packId);
  const lint = useLint(packId);
  const [busy, setBusy] = useState(false);
  const [newName, setNewName] = useState("");
  const [rename, setRename] = useState<string | null>(null);
  const [del, setDel] = useState(false);
  useEffect(() => setRename(null), [cur]);
  const diags = (lint.data?.diagnostics ?? []).filter((x: LintDiagnostic) => cur && (x.file === "layer/" + cur || x.file.endsWith("/" + cur) || x.file === cur));
  const run = async (f: () => Promise<unknown>, fail: string) => { setBusy(true); try { await f(); } catch (e) { toast.error(e, fail); } finally { setBusy(false); } };
  const create = () => {
    const p = layerPath(newName);
    if (!p) return;
    if (texts && p in texts) { setSel(p); setNewName(""); return; }
    void run(async () => { await write(p, EMPTY); setSel(p); setNewName(""); }, t("studio.data.saveFailed", { file: p }));
  };
  const save = () => cur && run(async () => { await write(cur, text); setDraft(key, null); }, t("studio.data.saveFailed", { file: cur }));
  const doRename = () => {
    const p = rename === null ? null : layerPath(rename);
    if (!cur || !p || p === cur) return;
    if (texts && p in texts) { toast.error(t("studio.data.raw.exists", { file: p })); return; }
    void run(async () => { await write(p, text); await remove(cur); setDraft(key, null); setSel(p); setRename(null); }, t("studio.data.saveFailed", { file: p }));
  };
  const newPath = newName.trim() ? layerPath(newName) : null;
  return (
    <div className="kd-raw" data-testid="data-raw">
      <aside className="kd-raw-files">
        <ul className="kd-raw-list" data-testid="data-raw-files">
          {files.map((f) => (
            <li key={f}><button type="button" className={"kd-raw-file" + (f === cur ? " on" : "")} onClick={() => setSel(f)} data-testid={"data-raw-file-" + f}>{f.slice(8)}</button></li>
          ))}
          {files.length === 0 && <li className="kst-dim">{t("studio.data.raw.none")}</li>}
        </ul>
        <form className="kd-raw-new" onSubmit={(e) => { e.preventDefault(); create(); }}>
          <TextInput mono value={newName} onChange={(e) => setNewName(e.target.value)} placeholder={t("studio.data.raw.newPh")} invalid={!!newName.trim() && !newPath}
            aria-label={t("studio.data.raw.newAria")} data-testid="data-raw-new-name" />
          <Button size="s" type="submit" disabled={!newPath || busy} data-testid="data-raw-new">{t("studio.data.raw.new")}</Button>
        </form>
      </aside>
      {cur ? (
        <section className="kd-raw-main">
          <div className="kd-savebar">
            {rename === null ? <span className="kst-mono" data-testid="data-raw-path">{cur}</span> : (
              <form className="kst-row" onSubmit={(e) => { e.preventDefault(); doRename(); }}>
                <TextInput mono value={rename} onChange={(e) => setRename(e.target.value)} invalid={!layerPath(rename)} aria-label={t("studio.data.raw.renameAria")} data-testid="data-raw-rename-name" autoFocus />
                <Button size="s" type="submit" disabled={!layerPath(rename) || busy} data-testid="data-raw-rename-ok">{t("studio.data.raw.renameOk")}</Button>
                <Button size="s" variant="ghost" onClick={() => setRename(null)}>{t("studio.data.discard")}</Button>
              </form>
            )}
            <span className={draft?.dirty ? "kst-warn-text" : "kst-dim"} data-testid="data-raw-state">{draft?.dirty ? t("studio.data.unsaved") : t("studio.data.saved")}</span>
            <span className="kd-grow" />
            {rename === null && <Button size="s" variant="ghost" onClick={() => setRename(cur.slice(8))} data-testid="data-raw-rename">{t("studio.data.raw.rename")}</Button>}
            <Button size="s" variant="ghost" onClick={() => setDel(true)} data-testid="data-raw-delete">{t("studio.data.raw.delete")}</Button>
            {draft?.dirty && <Button size="s" variant="ghost" onClick={() => setDraft(key, null)} data-testid="data-raw-discard">{t("studio.data.discard")}</Button>}
            <Button size="s" variant="primary" disabled={!draft?.dirty} loading={busy} onClick={() => void save()} data-testid="data-raw-save">{t("studio.data.save")}</Button>
          </div>
          <TextArea className="kd-raw-text" value={text} spellCheck={false} onChange={(e) => setDraft(key, { value: e.target.value, base: undefined, dirty: true })}
            aria-label={t("studio.data.raw.textAria", { file: cur })} data-testid="data-raw-text"
            onKeyDown={(e) => { if ((e.ctrlKey || e.metaKey) && e.key === "s") { e.preventDefault(); if (draft?.dirty) void save(); } }} />
          <DiagList diags={diags} empty={t("studio.data.raw.clean")} testId="data-raw-lint" />
        </section>
      ) : <p className="kst-dim">{t("studio.data.raw.pick")}</p>}
      {del && cur && (
        <ConfirmDanger open title={t("studio.data.raw.deleteTitle", { file: cur })} message={t("studio.data.raw.deleteText")} action={t("studio.data.raw.delete")} word={cur.slice(8)} relaxable
          onClose={() => setDel(false)} onConfirm={async () => { try { await remove(cur); setDraft(key, null); setSel(null); } catch (e) { toast.error(e); throw e; } }} />
      )}
    </div>
  );
}

/** Diagnostics as lines: severity, where, message. */
export function DiagList({ diags, empty, testId }: { diags: LintDiagnostic[]; empty?: string; testId: string }) {
  const t = useT();
  if (!diags.length) return empty ? <p className="kst-dim kd-note" data-testid={testId}>{empty}</p> : null;
  return (
    <ul className="kd-diags" data-testid={testId}>
      {diags.map((x, i) => (
        <li key={i} className={"kd-diag kd-diag-" + x.severity}>
          <span className={"kd-sev kd-sev-" + x.severity}>{x.severity === "error" ? t("studio.data.lint.error") : x.severity === "warning" ? t("studio.data.lint.warning") : t("studio.data.lint.info")}</span>
          <span className="kst-mono kst-dim">{x.file.replace(/^layer\//, "")}:{x.line}</span>
          <span className="kst-mono kst-dim">{x.code}</span>
          <span>{x.message}</span>
        </li>
      ))}
    </ul>
  );
}
