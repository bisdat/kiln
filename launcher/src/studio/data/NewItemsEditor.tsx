// New items: "add" clones (a fixed GUID, a debug name, the name players see, a base item from the game or the pack's
// mods, a few fields). The "add" list of the items section in content/items.json (the rules there, Items, stay).
import { X } from "lucide-react";
import { useT } from "../../i18n";
import { Button, IconButton, TextInput } from "../../ui";
import { FieldRows } from "./Fields";
import { NAME_RE, duplicates, isObj, itemDefValue, itemDefsOf, newGuid, readDoc, withSection, type ItemDef, type Obj } from "./model";
import { AlsoIn, SaveBar, otherFiles, useSaveFile } from "./parts";
import { ItemIcon, Picker, useDataCtx } from "./Picker";
import { useDraft, useFields } from "./store";

const PATH = "content/items.json";

export default function NewItemsEditor({ texts }: { texts: Record<string, string> | undefined }) {
  const t = useT();
  const { packId } = useDataCtx();
  const fq = useFields("items");
  const d = useDraft<ItemDef[]>(packId + ":added", texts?.[PATH], (text) => itemDefsOf(readDoc(text).value.items));
  const file = useSaveFile(packId, PATH, texts);
  const defs = d.value;
  const dup = duplicates(defs.map((x) => x.name));
  const problem = defs.some((x) => !NAME_RE.test(x.name.trim())) ? t("studio.data.badName")
    : dup.size ? t("studio.data.dupName", { name: [...dup][0] })
      : defs.some((x) => !x.base.trim()) ? t("studio.data.added.noBase") : null;
  const setAt = (i: number, x: ItemDef) => d.change(defs.map((y, j) => (j === i ? x : y)));
  const save = () => file.save((doc) => {
    const old = isObj(doc.items) ? (doc.items as Obj) : {};
    const { add: _add, ...rules } = old;
    return withSection(doc, "items", defs.length ? { add: defs.map(itemDefValue), ...rules } : rules);
  }, d.saved);
  return (
    <div className="kd-editor" data-testid="data-added">
      <p className="kst-p kst-dim">{t("studio.data.added.lead")}</p>
      <AlsoIn files={otherFiles(texts, PATH, "items", true)} />
      {defs.length === 0 && <p className="kst-dim">{t("studio.data.added.empty")}</p>}
      {defs.map((x, i) => (
        <div className="kd-card" key={x.guid || i} data-testid={"data-added-" + i}>
          <div className="kd-card-head">
            <ItemIcon name={x.base} size={36} />
            <TextInput mono className="kd-name" value={x.name} onChange={(e) => setAt(i, { ...x, name: e.target.value })} invalid={!!x.name && !NAME_RE.test(x.name)}
              placeholder="My_Item" aria-label={t("studio.data.name")} data-testid={"data-added-" + i + "-name"} />
            <span className="kst-dim kst-mono kd-guid" title={t("studio.data.guid")}>{x.guid}</span>
            <span className="kd-grow" />
            <IconButton label={t("studio.data.added.remove")} icon={<X size={16} />} onClick={() => d.change(defs.filter((_, j) => j !== i))} data-testid={"data-added-" + i + "-remove"} />
          </div>
          <div className="kd-line">
            <span className="kst-label kd-key">{t("studio.data.added.displayName")}</span>
            <TextInput className="kd-text" value={x.displayName} onChange={(e) => setAt(i, { ...x, displayName: e.target.value })} aria-label={t("studio.data.added.displayName")} data-testid={"data-added-" + i + "-display"} />
          </div>
          <div className="kd-line">
            <span className="kst-label kd-key">{t("studio.data.added.base")}</span>
            <Picker section="items" icons value={x.base} onChange={(v) => setAt(i, { ...x, base: v })} aria-label={t("studio.data.added.base")} testId={"data-added-" + i + "-base"} />
          </div>
          <div className="kd-line">
            <span className="kst-label kd-key">{t("studio.data.added.description")}</span>
            <TextInput className="kd-text" value={x.description} onChange={(e) => setAt(i, { ...x, description: e.target.value })} aria-label={t("studio.data.added.description")} data-testid={"data-added-" + i + "-desc"} />
          </div>
          <label className="kst-tick"><input type="checkbox" className="k-check" checked={x.rolledLoot} onChange={(e) => setAt(i, { ...x, rolledLoot: e.target.checked })} data-testid={"data-added-" + i + "-rolled"} />{t("studio.data.added.rolledLoot")}</label>
          <span className="kst-label">{t("studio.data.added.fields")}</span>
          <FieldRows fields={x.fields} infos={fq.data ?? []} onChange={(f) => setAt(i, { ...x, fields: f })} testId={"data-added-" + i} />
        </div>
      ))}
      <div className="kst-row">
        <Button onClick={() => d.change([...defs, { guid: newGuid(), name: "", base: "", displayName: "", description: "", rolledLoot: false, fields: [], rest: {} }])} data-testid="data-added-add">{t("studio.data.added.add")}</Button>
      </div>
      <SaveBar path={PATH} dirty={d.dirty} busy={file.busy} broken={file.broken} problem={problem} onSave={save} onDiscard={d.saved} testId="data-added" />
    </div>
  );
}
