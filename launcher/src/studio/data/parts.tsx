// Pieces the data editors share: the save bar of an editor's file, item stacks (recipes), "also set in" notes.
import { X } from "lucide-react";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { useT } from "../../i18n";
import { Button, IconButton, toast } from "../../ui";
import { NumBox } from "./Fields";
import { onlyCategoryOf, summary } from "./categories";
import { CategoryChip, CategoryPanel, CategoryPicker, useAnyOf } from "./CategoryPicker";
import { isCategoryStack, readDoc, writeDoc, type Obj, type Stack } from "./model";
import { Picker } from "./Picker";
import { useCategories, useShowHidden, useWriteLayer } from "./store";

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

/** Item stacks: [icon] picker  x count  remove; "+ input". With `categories` (inputs: outputs never take one) a stack may
 *  be "any of" a category of the game instead ("+ Any of a group"), and an item that only one category holds offers it. */
export function StackList({ stacks, onChange, testId, addLabel, categories }: { stacks: Stack[]; onChange: (s: Stack[]) => void; testId: string; addLabel: string; categories?: boolean }) {
  const t = useT();
  const cats = useCategories();
  const none = !cats.data?.length;
  const setAt = (i: number, s: Stack) => onChange(stacks.map((x, j) => (j === i ? s : x)));
  // stable row keys (a row's own state, as an expanded group, stays with its row when one above is removed); a list
  // replaced from outside (Discard, a file read again) gets new ones
  const keys = useRef<number[]>([]);
  const nextKey = useRef(0);
  if (keys.current.length !== stacks.length) keys.current = stacks.map(() => nextKey.current++);
  const add = (s: Stack) => { keys.current = [...keys.current, nextKey.current++]; onChange([...stacks, s]); };
  const remove = (i: number) => { keys.current = keys.current.filter((_, j) => j !== i); onChange(stacks.filter((_, j) => j !== i)); };
  return (
    <div className="kd-stacks">
      {stacks.map((s, i) => (
        <StackRow key={keys.current[i]} s={s} categories={!!categories} testId={testId + "-" + i} onChange={(x) => setAt(i, x)} onRemove={() => remove(i)} />
      ))}
      <div className="kd-stack-adds">
        <Button size="s" variant="ghost" disabled={stacks.length >= 32} onClick={() => add({ item: "", count: 1 })} data-testid={testId + "-add"}>{addLabel}</Button>
        {categories && (
          <Button size="s" variant="ghost" disabled={stacks.length >= 32 || none} title={none ? t("studio.data.recipes.categoryNoCatalog") : t("studio.data.recipes.addCategoryTip")}
            onClick={() => add({ item: "", count: 1, category: "" })} data-testid={testId + "-add-category"}>{t("studio.data.recipes.addCategory")}</Button>
        )}
      </div>
    </div>
  );
}

function StackRow({ s, categories, testId, onChange, onRemove }: { s: Stack; categories: boolean; testId: string; onChange: (s: Stack) => void; onRemove: () => void }) {
  const t = useT();
  const cats = useCategories();
  const showHidden = useShowHidden((x) => x.on);
  const anyOf = useAnyOf();
  const [open, setOpen] = useState(false);
  const row = useRef<HTMLDivElement>(null);
  // a group chosen in the picker or taken from the offer replaces the focused control: focus goes to the new chip's
  // expand button (the count box when the chip has none), not to the top of the page
  const focusChip = useRef(false);
  const chose = (x: Stack) => { focusChip.current = true; onChange(x); };
  useEffect(() => {
    setOpen(false);
    if (!focusChip.current || s.category === undefined || s.category === "") return;
    focusChip.current = false;
    const el = row.current?.querySelector<HTMLElement>("[data-cat-expand]") ?? row.current?.querySelector<HTMLElement>("[data-testid='" + testId + "-count']");
    el?.focus();
    // eslint-disable-next-line react-hooks/exhaustive-deps -- only when the row's group changes (not when rows above move)
  }, [s.category]);
  const cat = isCategoryStack(s);
  const offer = categories && !cat && s.item.trim() ? onlyCategoryOf(cats.data ?? [], s.item, showHidden) : null;
  return (
    <div className="kd-stack" ref={row} data-testid={testId}>
      {!cat ? <Picker section="items" icons value={s.item} onChange={(v) => onChange({ ...s, item: v })} aria-label={t("studio.data.recipes.itemAria")} testId={testId + "-item"} />
        : s.category === "" ? <CategoryPicker onChange={(id) => chose({ ...s, category: id })} testId={testId + "-category"} />
          : <CategoryChip id={s.category!} open={open} onToggle={() => setOpen(!open)} testId={testId + "-category"} />}
      <span className="kst-dim">×</span>
      <NumBox value={s.count} integer onCommit={(v) => onChange({ ...s, count: Math.max(1, Math.min(65535, v ?? 1)) })} aria-label={t("studio.data.recipes.countAria")} testId={testId + "-count"} />
      <IconButton label={cat ? t("studio.data.recipes.removeCategory") : t("studio.data.remove")} icon={<X size={15} />} size="s" onClick={onRemove} data-testid={testId + "-remove"} />
      {offer && (
        <Button size="s" variant="ghost" className="kd-cat-offer" title={summary(offer, showHidden, Infinity).all.join(", ")}
          aria-label={t("studio.data.recipes.offerAria", { anyOf: anyOf(offer) })}
          onClick={() => chose({ item: "", count: s.count, category: offer.id })} data-testid={testId + "-offer"}>
          {t("studio.data.recipes.offer")}
        </Button>
      )}
      {cat && open && s.category !== "" && <CategoryPanel id={s.category!} testId={testId + "-category"} />}
    </div>
  );
}
