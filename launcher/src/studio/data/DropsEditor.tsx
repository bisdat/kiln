// Drops: default-inventory tables (enemy loot, fixed chests), by table GUID or a glob: amounts and chance scaled, and
// stacks of single items (countMin, countMax, spawnProbability, weight). content/drops.json, the drops section.
import { X } from "lucide-react";
import type { Json } from "../../api/commands";
import { useT } from "../../i18n";
import { Button, IconButton } from "../../ui";
import { FieldRows, ValueEdit } from "./Fields";
import { FALLBACK_FIELDS, dropRuleBody, dropRulesOf, duplicates, mergeFields, readDoc, withSection, type DropRule, type Obj } from "./model";
import { AlsoIn, SaveBar, otherFiles, useSaveFile } from "./parts";
import { Picker, useDataCtx } from "./Picker";
import { useDraft, useFields } from "./store";

const PATH = "content/drops.json";
const NUM = { path: "value", type: "float32", writable: true, kind: "float32" };

export default function DropsEditor({ texts }: { texts: Record<string, string> | undefined }) {
  const t = useT();
  const { packId } = useDataCtx();
  const fq = useFields("drops");
  const infos = mergeFields(FALLBACK_FIELDS.dropItems, (fq.data ?? []).filter((f) => !["chance", "items", "count"].includes(f.path)));
  const d = useDraft<DropRule[]>(packId + ":drops", texts?.[PATH], (text) => dropRulesOf(readDoc(text).value.drops));
  const file = useSaveFile(packId, PATH, texts);
  const rules = d.value;
  const dup = duplicates(rules.map((r) => r.sel));
  const problem = rules.some((r) => !r.sel.trim()) ? t("studio.data.drops.noTable") : dup.size ? t("studio.data.rules.duplicate", { selector: [...dup][0] }) : null;
  const setAt = (i: number, r: DropRule) => d.change(rules.map((x, j) => (j === i ? r : x)));
  const save = () => file.save((doc) => {
    const body: Obj = {};
    for (const r of rules) body[r.sel.trim()] = dropRuleBody(r);
    return withSection(doc, "drops", body);
  }, d.saved);
  const opt = (r: DropRule, i: number, key: "count" | "chance", start: Json, label: string) => (
    <div className="kd-line">
      <label className="kst-tick kd-key"><input type="checkbox" className="k-check" checked={r[key] !== undefined} onChange={(e) => setAt(i, { ...r, [key]: e.target.checked ? start : undefined })}
        data-testid={"data-drops-" + i + "-" + key + "-on"} />{label}</label>
      {r[key] !== undefined && <ValueEdit info={NUM} value={r[key]} onChange={(v) => setAt(i, { ...r, [key]: v })} label={label} testId={"data-drops-" + i + "-" + key} />}
    </div>
  );
  return (
    <div className="kd-editor" data-testid="data-drops">
      <p className="kst-p kst-dim">{t("studio.data.drops.lead")}</p>
      <AlsoIn files={otherFiles(texts, PATH, "drops")} />
      {rules.length === 0 && <p className="kst-dim">{t("studio.data.drops.empty")}</p>}
      {rules.map((r, i) => (
        <div className="kd-card" key={i} data-testid={"data-drops-rule-" + i}>
          <div className="kd-card-head">
            <span className="kst-label">{t("studio.data.drops.table")}</span>
            <Picker section="drops" free value={r.sel} pickValue={(h) => h.id ?? h.name} onChange={(v) => setAt(i, { ...r, sel: v })} aria-label={t("studio.data.drops.table")} testId={"data-drops-" + i + "-sel"} />
            <span className="kd-grow" />
            <IconButton label={t("studio.data.rules.remove")} icon={<X size={16} />} onClick={() => d.change(rules.filter((_, j) => j !== i))} data-testid={"data-drops-" + i + "-remove"} />
          </div>
          {opt(r, i, "count", { mul: 2 }, t("studio.data.drops.count"))}
          {opt(r, i, "chance", { mul: 1.5, max: 1 }, t("studio.data.drops.chance"))}
          <span className="kst-label">{t("studio.data.drops.items")}</span>
          {r.items.map(([item, fields], k) => (
            <div className="kd-sub" key={k} data-testid={"data-drops-" + i + "-item-" + k}>
              <div className="kd-card-head">
                <Picker section="items" icons free value={item} onChange={(v) => setAt(i, { ...r, items: r.items.map((x, j) => (j === k ? [v, x[1]] : x)) })} aria-label={t("studio.data.recipes.itemAria")} testId={"data-drops-" + i + "-item-" + k + "-sel"} />
                <span className="kd-grow" />
                <IconButton label={t("studio.data.remove")} icon={<X size={15} />} size="s" onClick={() => setAt(i, { ...r, items: r.items.filter((_, j) => j !== k) })} data-testid={"data-drops-" + i + "-item-" + k + "-remove"} />
              </div>
              <FieldRows fields={fields} infos={infos} onChange={(f) => setAt(i, { ...r, items: r.items.map((x, j) => (j === k ? [x[0], f] : x)) })} testId={"data-drops-" + i + "-item-" + k} />
            </div>
          ))}
          <div className="kst-row">
            <Button size="s" variant="ghost" onClick={() => setAt(i, { ...r, items: [...r.items, ["", [["countMax", 5]]]] })} data-testid={"data-drops-" + i + "-additem"}>{t("studio.data.drops.addItem")}</Button>
          </div>
        </div>
      ))}
      <div className="kst-row">
        <Button onClick={() => d.change([...rules, { sel: "", count: undefined, chance: undefined, items: [], rest: {} }])} data-testid="data-drops-add">{t("studio.data.drops.add")}</Button>
        <Button variant="ghost" disabled={rules.some((r) => r.sel === "*")} onClick={() => d.change([...rules, { sel: "*", count: { mul: 2 }, chance: undefined, items: [], rest: {} }])}
          data-testid="data-drops-add-all">{t("studio.data.drops.addAll")}</Button>
      </div>
      <SaveBar path={PATH} dirty={d.dirty} busy={file.busy} broken={file.broken} problem={problem} onSave={save} onDiscard={d.saved} testId="data-drops" />
    </div>
  );
}
