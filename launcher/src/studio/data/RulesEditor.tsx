// Items and Loot: a rule per selector (an item name or a glob like "Material_T1_*", "*"), optional @where conditions,
// and the fields it sets (typed from the catalog's FieldInfo). Items write the items section of content/items.json
// (its "add" list, New items, stays); Loot the loot section of content/loot.json.
import { X } from "lucide-react";
import type { Json } from "../../api/commands";
import { useT } from "../../i18n";
import { Button, IconButton } from "../../ui";
import { FieldRows, WhereRows } from "./Fields";
import { FALLBACK_FIELDS, duplicates, isObj, mergeFields, readDoc, ruleBody, rulesOf, withSection, type Obj, type Rule } from "./model";
import { AlsoIn, SaveBar, otherFiles, useSaveFile } from "./parts";
import { Picker, useDataCtx } from "./Picker";
import { useDraft, useFields } from "./store";

const SPEC = {
  items: { path: "content/items.json", start: [["maxStackSize", 999]] as [string, Json][] },
  loot: { path: "content/loot.json", start: [["stackSizeMin", 1], ["stackSizeMax", 10]] as [string, Json][] },
};

export default function RulesEditor({ section, texts }: { section: "items" | "loot"; texts: Record<string, string> | undefined }) {
  const t = useT();
  const { packId } = useDataCtx();
  const { path, start } = SPEC[section];
  const fq = useFields(section);
  const infos = mergeFields(fq.data ?? [], section === "loot" ? FALLBACK_FIELDS.loot : []);
  const d = useDraft<Rule[]>(packId + ":" + section, texts?.[path], (text) => rulesOf(readDoc(text).value[section]));
  const file = useSaveFile(packId, path, texts);
  const rules = d.value;
  const dup = duplicates(rules.map((r) => r.sel));
  const problem = rules.some((r) => !r.sel.trim()) ? t("studio.data.rules.noSelector") : dup.size ? t("studio.data.rules.duplicate", { selector: [...dup][0] }) : null;
  const setAt = (i: number, r: Rule) => d.change(rules.map((x, j) => (j === i ? r : x)));
  const save = () => file.save((doc) => {
    const old = isObj(doc[section]) ? (doc[section] as Obj) : {};
    const body: Obj = {};
    if (old.add !== undefined) body.add = old.add;
    for (const r of rules) body[r.sel.trim()] = ruleBody(r);
    return withSection(doc, section, body);
  }, d.saved);
  const tid = "data-" + section;
  return (
    <div className="kd-editor" data-testid={tid}>
      <p className="kst-p kst-dim">{section === "items" ? t("studio.data.items.lead") : t("studio.data.loot.lead")}</p>
      <AlsoIn files={otherFiles(texts, path, section)} />
      {rules.length === 0 && <p className="kst-dim" data-testid={tid + "-empty"}>{section === "items" ? t("studio.data.items.empty") : t("studio.data.loot.empty")}</p>}
      {rules.map((r, i) => (
        <div className="kd-card" key={i} data-testid={tid + "-rule-" + i}>
          <div className="kd-card-head">
            <span className="kst-label">{section === "items" ? t("studio.data.items.selector") : t("studio.data.loot.selector")}</span>
            <Picker section={section} free icons value={r.sel} onChange={(v) => setAt(i, { ...r, sel: v })} aria-label={t("studio.data.rules.selectorAria")} testId={tid + "-" + i + "-sel"} />
            <span className="kd-grow" />
            <IconButton label={t("studio.data.rules.remove")} icon={<X size={16} />} onClick={() => d.change(rules.filter((_, j) => j !== i))} data-testid={tid + "-" + i + "-remove"} />
          </div>
          <WhereRows where={r.where} infos={infos} onChange={(w) => setAt(i, { ...r, where: w })} testId={tid + "-" + i} />
          <FieldRows fields={r.fields} infos={infos} onChange={(f) => setAt(i, { ...r, fields: f })} testId={tid + "-" + i} />
          {Object.keys(r.extra).length > 0 && <p className="kst-dim kst-mono kd-note">{Object.keys(r.extra).join(", ")}</p>}
        </div>
      ))}
      <div className="kst-row">
        <Button onClick={() => d.change([...rules, { sel: "", where: [], fields: start, extra: {} }])} data-testid={tid + "-add"}>{t("studio.data.rules.add")}</Button>
        <Button variant="ghost" disabled={rules.some((r) => r.sel === "*")} onClick={() => d.change([...rules, { sel: "*", where: section === "items" ? [["category", "Materials"]] : [],
            fields: section === "items" ? [["maxStackSize", { mul: 4, max: 9999, raiseOnly: true }]] : start, extra: {} }])}
          data-testid={tid + "-add-all"}>{t("studio.data.rules.addAll")}</Button>
      </div>
      <SaveBar path={path} dirty={d.dirty} busy={file.busy} broken={file.broken} problem={problem} onSave={save} onDiscard={d.saved} testId={tid} />
    </div>
  );
}
