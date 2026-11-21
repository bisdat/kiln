// Recipes: changes to recipes of the game or the pack's mods (inputs, outputs, crafting duration) and new recipes ("add":
// a station, inputs, outputs, duration). content/recipes.json, the recipes section.
import { X } from "lucide-react";
import { useT } from "../../i18n";
import { Button, IconButton, TextInput } from "../../ui";
import { NumBox } from "./Fields";
import { NAME_RE, duplicates, isObj, newGuid, readDoc, recipeDefValue, recipeDefsOf, recipeRuleBody, recipeRulesOf, withSection, type Obj, type RecipeDef, type RecipeRule, type Stack } from "./model";
import { AlsoIn, SaveBar, StackList, otherFiles, useSaveFile } from "./parts";
import { Picker, useDataCtx } from "./Picker";
import { useDraft, useEntry } from "./store";

const PATH = "content/recipes.json";
interface Value { rules: RecipeRule[]; defs: RecipeDef[] }

export default function RecipesEditor({ texts }: { texts: Record<string, string> | undefined }) {
  const t = useT();
  const { packId } = useDataCtx();
  const d = useDraft<Value>(packId + ":recipes", texts?.[PATH], (text) => {
    const s = readDoc(text).value.recipes;
    return { rules: recipeRulesOf(s), defs: recipeDefsOf(s) };
  });
  const file = useSaveFile(packId, PATH, texts);
  const { rules, defs } = d.value;
  const dup = duplicates(rules.map((r) => r.sel));
  const dupNames = duplicates(defs.map((x) => x.name));
  const problem = rules.some((r) => !r.sel.trim()) ? t("studio.data.recipes.noRecipe")
    : dup.size ? t("studio.data.rules.duplicate", { selector: [...dup][0] })
      : defs.some((x) => !NAME_RE.test(x.name.trim())) ? t("studio.data.badName")
        : dupNames.size ? t("studio.data.dupName", { name: [...dupNames][0] })
          : defs.some((x) => !x.station.trim()) ? t("studio.data.recipes.noStation")
            : defs.some((x) => !x.inputs.some((s) => s.item.trim() || (s.category !== undefined && s.category !== "")) || !x.outputs.some((s) => s.item.trim())) ? t("studio.data.recipes.needStacks")
              : [...defs.map((x) => x.inputs), ...rules.map((r) => r.inputs ?? [])].some((l) => l.some((s) => s.category === "" && !s.item.trim())) ? t("studio.data.recipes.noCategory")
                : null;
  const setRule = (i: number, r: RecipeRule) => d.change({ defs, rules: rules.map((x, j) => (j === i ? r : x)) });
  const setDef = (i: number, x: RecipeDef) => d.change({ rules, defs: defs.map((y, j) => (j === i ? x : y)) });
  const save = () => file.save((doc) => {
    const old = isObj(doc.recipes) ? (doc.recipes as Obj) : {};
    const body: Obj = {};
    if (defs.length) body.add = defs.map(recipeDefValue);
    else if (old.add !== undefined && !Array.isArray(old.add)) body.add = old.add;
    for (const r of rules) body[r.sel.trim()] = recipeRuleBody(r);
    return withSection(doc, "recipes", body);
  }, d.saved);
  return (
    <div className="kd-editor" data-testid="data-recipes">
      <p className="kst-p kst-dim">{t("studio.data.recipes.lead")}</p>
      <AlsoIn files={otherFiles(texts, PATH, "recipes")} />
      <h3 className="kst-h3">{t("studio.data.recipes.changes")}</h3>
      {rules.length === 0 && <p className="kst-dim">{t("studio.data.recipes.noChanges")}</p>}
      {rules.map((r, i) => <RuleCard key={i} i={i} r={r} onChange={(x) => setRule(i, x)} onRemove={() => d.change({ defs, rules: rules.filter((_, j) => j !== i) })} />)}
      <div className="kst-row">
        <Button onClick={() => d.change({ defs, rules: [...rules, { sel: "", inputs: null, outputs: null, duration: null, rest: {} }] })} data-testid="data-recipes-add">{t("studio.data.recipes.add")}</Button>
      </div>
      <h3 className="kst-h3">{t("studio.data.recipes.new")}</h3>
      {defs.length === 0 && <p className="kst-dim">{t("studio.data.recipes.noNew")}</p>}
      {defs.map((x, i) => (
        <div className="kd-card" key={x.guid || i} data-testid={"data-recipes-new-" + i}>
          <div className="kd-card-head">
            <span className="kst-label">{t("studio.data.name")}</span>
            <TextInput mono className="kd-name" value={x.name} onChange={(e) => setDef(i, { ...x, name: e.target.value })} invalid={!!x.name && !NAME_RE.test(x.name)}
              aria-label={t("studio.data.name")} data-testid={"data-recipes-new-" + i + "-name"} placeholder="My_Recipe" />
            <span className="kst-dim kst-mono kd-guid" title={t("studio.data.guid")}>{x.guid}</span>
            <span className="kd-grow" />
            <IconButton label={t("studio.data.recipes.removeNew")} icon={<X size={16} />} onClick={() => d.change({ rules, defs: defs.filter((_, j) => j !== i) })} data-testid={"data-recipes-new-" + i + "-remove"} />
          </div>
          <div className="kd-line">
            <span className="kst-label kd-key">{t("studio.data.recipes.station")}</span>
            <Picker section="workshops" free value={x.station} pickValue={(h) => h.id ?? h.name} onChange={(v) => setDef(i, { ...x, station: v })}
              placeholder={t("studio.data.recipes.stationPh")} aria-label={t("studio.data.recipes.station")} testId={"data-recipes-new-" + i + "-station"} />
            <Button size="s" variant="ghost" onClick={() => setDef(i, { ...x, station: "hand" })} data-testid={"data-recipes-new-" + i + "-hand"}>{t("studio.data.recipes.hand")}</Button>
          </div>
          <Stacks label={t("studio.data.recipes.inputs")} stacks={x.inputs} onChange={(s) => setDef(i, { ...x, inputs: s })} testId={"data-recipes-new-" + i + "-in"} add={t("studio.data.recipes.addInput")} categories />
          <Stacks label={t("studio.data.recipes.outputs")} stacks={x.outputs} onChange={(s) => setDef(i, { ...x, outputs: s })} testId={"data-recipes-new-" + i + "-out"} add={t("studio.data.recipes.addOutput")} />
          <Duration value={x.duration} onChange={(v) => setDef(i, { ...x, duration: v })} testId={"data-recipes-new-" + i + "-duration"} />
        </div>
      ))}
      <div className="kst-row">
        <Button onClick={() => d.change({ rules, defs: [...defs, { guid: newGuid(), name: "", station: "hand", inputs: [{ item: "", count: 1 }], outputs: [{ item: "", count: 1 }], duration: null, rest: {} }] })}
          data-testid="data-recipes-new">{t("studio.data.recipes.addNew")}</Button>
      </div>
      <SaveBar path={PATH} dirty={d.dirty} busy={file.busy} broken={file.broken} problem={problem} onSave={save} onDiscard={d.saved} testId="data-recipes" />
    </div>
  );
}

function Stacks({ label, stacks, onChange, testId, add, categories }: { label: string; stacks: Stack[]; onChange: (s: Stack[]) => void; testId: string; add: string; categories?: boolean }) {
  return (
    <div className="kd-line kd-top">
      <span className="kst-label kd-key">{label}</span>
      <StackList stacks={stacks} onChange={onChange} testId={testId} addLabel={add} categories={categories} />
    </div>
  );
}

function Duration({ value, onChange, testId }: { value: number | null; onChange: (v: number | null) => void; testId: string }) {
  const t = useT();
  return (
    <div className="kd-line">
      <span className="kst-label kd-key">{t("studio.data.recipes.duration")}</span>
      <NumBox value={value ?? undefined} onCommit={(v) => onChange(v === undefined ? null : Math.max(0, v))} placeholder={t("studio.data.recipes.durationPh")} aria-label={t("studio.data.recipes.duration")} testId={testId} />
      <span className="kst-dim">{t("studio.data.recipes.seconds")}</span>
    </div>
  );
}

/** One change to a recipe: which, then inputs / outputs / duration, each only when ticked. */
function RuleCard({ i, r, onChange, onRemove }: { i: number; r: RecipeRule; onChange: (r: RecipeRule) => void; onRemove: () => void }) {
  const t = useT();
  const { packId, fromName } = useDataCtx();
  const entry = useEntry(packId, "recipes", r.sel);
  const mod = entry?.from && entry.from !== "pack." + packId ? fromName(entry.from) : null;
  const tid = "data-recipes-rule-" + i;
  const part = (on: boolean, which: "inputs" | "outputs") => onChange({ ...r, [which]: on ? [{ item: "", count: 1 }] : null });
  return (
    <div className="kd-card" data-testid={tid}>
      <div className="kd-card-head">
        <span className="kst-label">{t("studio.data.recipes.recipe")}</span>
        <Picker section="recipes" free value={r.sel} onChange={(v) => onChange({ ...r, sel: v })} aria-label={t("studio.data.recipes.recipe")} testId={tid + "-sel"} />
        <span className="kd-grow" />
        <IconButton label={t("studio.data.rules.remove")} icon={<X size={16} />} onClick={onRemove} data-testid={tid + "-remove"} />
      </div>
      <label className="kst-tick"><input type="checkbox" className="k-check" checked={!!r.inputs} onChange={(e) => part(e.target.checked, "inputs")} data-testid={tid + "-in-on"} />{t("studio.data.recipes.changeInputs")}</label>
      {r.inputs && <StackList stacks={r.inputs} onChange={(s) => onChange({ ...r, inputs: s })} testId={tid + "-in"} addLabel={t("studio.data.recipes.addInput")} categories />}
      <label className="kst-tick"><input type="checkbox" className="k-check" checked={!!r.outputs} onChange={(e) => part(e.target.checked, "outputs")} data-testid={tid + "-out-on"} />{t("studio.data.recipes.changeOutputs")}</label>
      {r.outputs && <StackList stacks={r.outputs} onChange={(s) => onChange({ ...r, outputs: s })} testId={tid + "-out"} addLabel={t("studio.data.recipes.addOutput")} />}
      <Duration value={r.duration} onChange={(v) => onChange({ ...r, duration: v })} testId={tid + "-duration"} />
      {Object.keys(r.rest).length > 0 && <p className="kst-dim kst-mono kd-note">{Object.keys(r.rest).join(", ")}</p>}
      <p className="kst-dim kd-note" data-testid={tid + "-note"}>{mod ? t("studio.data.recipes.modNote", { mod }) : t("studio.data.recipes.stationNote")}</p>
    </div>
  );
}
