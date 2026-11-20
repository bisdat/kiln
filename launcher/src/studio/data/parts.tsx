// Pieces the data editors share: the save bar of an editor's file, item stacks (recipes), "also set in" notes.
import { X } from "lucide-react";
import { useState, type ReactNode } from "react";
import { useT } from "../../i18n";
import { Button, IconButton, toast } from "../../ui";
import { NumBox } from "./Fields";
import { readDoc, writeDoc, type Obj, type Stack } from "./model";
import { Picker } from "./Picker";
import { useWriteLayer } from "./store";

/** Saves an editor's part of its file: reads the file as it is now, lets the editor change it, writes it back. */
export function useSaveFile(packId: string, path: string, texts: Record<string, string> | undefined) {
  const t = useT();
  const { write } = useWriteLayer(packId);
  const [busy, setBusy] = useState(false);
  const doc = readDoc(texts?.[path]);
  const save = async (change: (doc: Obj) => Obj, done: () => void) => {
    if (!doc.ok) return;
    setBusy(true);
    try { await write(path, writeDoc(change(doc.value))); done(); } catch (e) { toast.error(e, t("studio.data.saveFailed", { file: path })); } finally { setBusy(false); }
  };
  return { broken: !doc.ok, busy, save, doc: doc.value };
}

/** The file an editor writes, its state and Save / Discard. */
export function SaveBar({ path, dirty, busy, broken, problem, onSave, onDiscard, testId, children }: {
  path: string; dirty: boolean; busy: boolean; broken: boolean; problem?: string | null; onSave: () => void; onDiscard: () => void; testId: string; children?: ReactNode;
}) {
  const t = useT();
  return (
    <div className="kd-savebar" data-testid={testId + "-bar"}>
      <span className="kst-mono kst-dim">{path}</span>
      {broken ? <span className="kst-check-err" data-testid={testId + "-broken"}>{t("studio.data.broken")}</span>
        : problem ? <span className="kst-warn-text" data-testid={testId + "-problem"}>{problem}</span>
          : <span className={dirty ? "kst-warn-text" : "kst-dim"} data-testid={testId + "-state"}>{dirty ? t("studio.data.unsaved") : t("studio.data.saved")}</span>}
      <span className="kd-grow" />
      {children}
      {dirty && <Button size="s" variant="ghost" onClick={onDiscard} data-testid={testId + "-discard"}>{t("studio.data.discard")}</Button>}
      <Button size="s" variant="primary" disabled={!dirty || broken || !!problem} loading={busy} onClick={onSave} data-testid={testId + "-save"}>{t("studio.data.save")}</Button>
    </div>
  );
}

/** "Also set in content/pack.json" (rules of the same section in other files: Raw JSON). */
export function AlsoIn({ files }: { files: string[] }) {
  const t = useT();
  if (!files.length) return null;
  return <p className="kst-dim kd-note" data-testid="data-also">{t("studio.data.alsoIn", { files: files.join(", ") })}</p>;
}
/** Other files of the layer whose section has rules (not "add"). */
export function otherFiles(texts: Record<string, string> | undefined, own: string, section: string, adds = false): string[] {
  return Object.entries(texts ?? {}).filter(([p, text]) => {
    if (p === own) return false;
    const d = readDoc(text);
    const s = d.value[section];
    return d.ok && !!s && typeof s === "object" && !Array.isArray(s) && Object.keys(s).some((k) => (adds ? k === "add" : k !== "add"));
  }).map(([p]) => p);
}

/** Item stacks: [icon] picker  x count  remove; "+ input". */
export function StackList({ stacks, onChange, testId, addLabel }: { stacks: Stack[]; onChange: (s: Stack[]) => void; testId: string; addLabel: string }) {
  const t = useT();
  const setAt = (i: number, s: Stack) => onChange(stacks.map((x, j) => (j === i ? s : x)));
  return (
    <div className="kd-stacks">
      {stacks.map((s, i) => (
        <div className="kd-stack" key={i} data-testid={testId + "-" + i}>
          {s.category && !s.item ? <span className="kst-mono kd-cat">{t("studio.data.recipes.category", { id: s.category })}</span>
            : <Picker section="items" icons value={s.item} onChange={(v) => setAt(i, { ...s, item: v })} aria-label={t("studio.data.recipes.itemAria")} testId={testId + "-" + i + "-item"} />}
          <span className="kst-dim">×</span>
          <NumBox value={s.count} integer onCommit={(v) => setAt(i, { ...s, count: Math.max(1, Math.min(65535, v ?? 1)) })} aria-label={t("studio.data.recipes.countAria")} testId={testId + "-" + i + "-count"} />
          <IconButton label={t("studio.data.remove")} icon={<X size={15} />} size="s" onClick={() => onChange(stacks.filter((_, j) => j !== i))} data-testid={testId + "-" + i + "-remove"} />
        </div>
      ))}
      <Button size="s" variant="ghost" disabled={stacks.length >= 32} onClick={() => onChange([...stacks, { item: "", count: 1 }])} data-testid={testId + "-add"}>{addLabel}</Button>
    </div>
  );
}
