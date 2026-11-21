// Typed controls for field values (from the catalog's FieldInfo: numbers with "x4, max 9999, raise only", enums, bools),
// the list of fields a rule sets, and its @where conditions.
import { useEffect, useId, useState } from "react";
import { X } from "lucide-react";
import type { CatalogFieldInfo, Json } from "../../api/commands";
import { useT } from "../../i18n";
import { Button, IconButton, Select, TextInput } from "../../ui";
import { kindOf, numOp, opValue, parseLoose, showLoose, type NumOp } from "./model";

/** A number box that commits on blur / Enter; empty = none. */
export function NumBox({ value, onCommit, integer, placeholder, testId, ...rest }: { value: number | undefined; onCommit: (v: number | undefined) => void; integer?: boolean; placeholder?: string; testId?: string; "aria-label"?: string }) {
  const [text, setText] = useState(value === undefined ? "" : String(value));
  useEffect(() => setText(value === undefined ? "" : String(value)), [value]);
  const n = text.trim() === "" ? undefined : Number(text);
  const bad = n !== undefined && (Number.isNaN(n) || (integer && !Number.isInteger(n)));
  const commit = () => { if (!bad && n !== value) onCommit(n); };
  return <input className={"k-input k-num kd-num" + (bad ? " invalid" : "")} inputMode={integer ? "numeric" : "decimal"} value={text} placeholder={placeholder}
    onChange={(e) => setText(e.target.value)} onBlur={commit} onKeyDown={(e) => { if (e.key === "Enter") commit(); }} aria-label={rest["aria-label"]} aria-invalid={bad || undefined} data-testid={testId} />;
}

export function defaultValue(f: CatalogFieldInfo | undefined): Json {
  const k = kindOf(f);
  return k === "integer" || k === "number" ? 1 : k === "enum" ? f!.enum![0] : k === "bool" ? true : "";
}

/** One value: typed by the field's kind; anything the controls cannot show stays as JSON text. */
export function ValueEdit({ info, value, onChange, testId, label }: { info?: CatalogFieldInfo; value: Json | undefined; onChange: (v: Json) => void; testId?: string; label: string }) {
  const t = useT();
  const kind = kindOf(info);
  const op = numOp(value);
  if ((kind === "integer" || kind === "number" || (kind === "other" && op && op.mode !== "set")) && (op || value === undefined)) {
    const o: NumOp = op ?? { mode: "set", value: 1 };
    const set = (p: Partial<NumOp>) => onChange(opValue({ ...o, ...p }));
    const rel = o.mode !== "set";
    return (
      <span className="kd-value" data-testid={testId}>
        <Select className="kd-mode" value={o.mode} onChange={(m) => set({ mode: m as NumOp["mode"], ...(m === "set" ? { raiseOnly: undefined, lowerOnly: undefined } : {}) })}
          aria-label={t("studio.data.value.modeAria", { field: label })} data-testid={testId ? testId + "-mode" : undefined}
          options={[{ value: "set", label: t("studio.data.value.set") }, { value: "mul", label: t("studio.data.value.mul") }, { value: "add", label: t("studio.data.value.add") }]} />
        <NumBox value={o.value} integer={kind === "integer" && o.mode !== "mul"} onCommit={(v) => set({ value: v ?? 0 })} aria-label={label} testId={testId ? testId + "-n" : undefined} />
        {rel && <>
          <NumBox value={o.min} onCommit={(v) => set({ min: v })} placeholder={t("studio.data.value.min")} aria-label={t("studio.data.value.minAria", { field: label })} testId={testId ? testId + "-min" : undefined} />
          <NumBox value={o.max} onCommit={(v) => set({ max: v })} placeholder={t("studio.data.value.max")} aria-label={t("studio.data.value.maxAria", { field: label })} testId={testId ? testId + "-max" : undefined} />
          <label className="kst-tick kd-tick"><input type="checkbox" className="k-check" checked={!!o.raiseOnly} onChange={(e) => set({ raiseOnly: e.target.checked || undefined, lowerOnly: undefined })}
            data-testid={testId ? testId + "-raise" : undefined} />{t("studio.data.value.raiseOnly")}</label>
          <label className="kst-tick kd-tick"><input type="checkbox" className="k-check" checked={!!o.lowerOnly} onChange={(e) => set({ lowerOnly: e.target.checked || undefined, raiseOnly: undefined })}
            data-testid={testId ? testId + "-lower" : undefined} />{t("studio.data.value.lowerOnly")}</label>
        </>}
      </span>
    );
  }
  if (kind === "enum" && (value === undefined || typeof value === "string" || typeof value === "number")) {
    const opts = [...info!.enum!];
    if (value !== undefined && !opts.includes(String(value))) opts.push(String(value));
    return <Select className="kd-enum" value={String(value ?? opts[0])} onChange={onChange} options={opts} aria-label={label} data-testid={testId} />;
  }
  if (kind === "bool" && (value === undefined || typeof value === "boolean")) {
    return <label className="kst-tick"><input type="checkbox" className="k-check" checked={value === true} onChange={(e) => onChange(e.target.checked)} aria-label={label} data-testid={testId} />{value ? t("studio.data.value.on") : t("studio.data.value.off")}</label>;
  }
  return <LooseBox value={value} onChange={onChange} label={label} testId={testId} />;
}

/** A value typed as text (numbers, true / false, JSON, else a string). */
function LooseBox({ value, onChange, label, testId }: { value: Json | undefined; onChange: (v: Json) => void; label: string; testId?: string }) {
  const [text, setText] = useState(showLoose(value));
  useEffect(() => setText(showLoose(value)), [value]);
  const commit = () => { if (text !== showLoose(value)) onChange(parseLoose(text)); };
  return <TextInput mono className="kd-loose" value={text} onChange={(e) => setText(e.target.value)} onBlur={commit} onKeyDown={(e) => { if (e.key === "Enter") commit(); }} aria-label={label} data-testid={testId} />;
}

/** The fields a rule sets: field, value, remove; "+ field". */
export function FieldRows({ fields, infos, onChange, testId, extraNames = [] }: { fields: [string, Json][]; infos: CatalogFieldInfo[]; onChange: (f: [string, Json][]) => void; testId: string; extraNames?: string[] }) {
  const t = useT();
  const writable = infos.filter((f) => f.writable !== false);
  const info = (p: string) => infos.find((f) => f.path === p);
  const used = new Set(fields.map(([k]) => k));
  const free = writable.filter((f) => !used.has(f.path));
  const names = (cur: string) => {
    const list = writable.map((f) => f.path).filter((p) => p === cur || !used.has(p));
    for (const n of extraNames) if (!list.includes(n) && (n === cur || !used.has(n))) list.push(n);
    if (cur && !list.includes(cur)) list.unshift(cur);
    return list;
  };
  const setAt = (i: number, row: [string, Json]) => onChange(fields.map((r, j) => (j === i ? row : r)));
  return (
    <div className="kd-fields">
      {fields.map(([k, v], i) => (
        <div className="kd-field" key={i}>
          {infos.length || extraNames.length ? (
            <Select className="kd-fname" value={k} onChange={(n) => setAt(i, [n, defaultValue(info(n))])} options={names(k)} aria-label={t("studio.data.field.nameAria")} data-testid={testId + "-f" + i + "-name"} />
          ) : (
            <TextInput mono className="kd-fname" defaultValue={k} onBlur={(e) => e.target.value !== k && setAt(i, [e.target.value.trim(), v])} aria-label={t("studio.data.field.nameAria")} data-testid={testId + "-f" + i + "-name"} />
          )}
          <ValueEdit info={info(k)} value={v} onChange={(nv) => setAt(i, [k, nv])} label={k} testId={testId + "-f" + i} />
          <IconButton label={t("studio.data.field.remove", { field: k })} icon={<X size={15} />} size="s" onClick={() => onChange(fields.filter((_, j) => j !== i))} data-testid={testId + "-f" + i + "-remove"} />
        </div>
      ))}
      <Button size="s" variant="ghost" onClick={() => { const f = free[0] ?? (extraNames.find((n) => !used.has(n)) ? { path: extraNames.find((n) => !used.has(n))! } as CatalogFieldInfo : null); onChange([...fields, [f?.path ?? "", defaultValue(f ? info(f.path) : undefined)]]); }}
        data-testid={testId + "-addfield"}>{t("studio.data.field.add")}</Button>
    </div>
  );
}

/** "@where" conditions: field, condition (a value, ">1", "!Name", a list as JSON). */
export function WhereRows({ where, infos, onChange, testId }: { where: [string, Json][]; infos: CatalogFieldInfo[]; onChange: (w: [string, Json][]) => void; testId: string }) {
  const t = useT();
  const dl = useId();
  const setAt = (i: number, row: [string, Json]) => onChange(where.map((r, j) => (j === i ? row : r)));
  return (
    <div className="kd-where">
      <datalist id={dl}>{["name", "category", ...infos.map((f) => f.path)].map((p) => <option key={p} value={p} />)}</datalist>
      {where.map(([k, v], i) => (
        <div className="kd-field" key={i}>
          <span className="kst-dim kd-where-word">{i === 0 ? t("studio.data.where.where") : t("studio.data.where.and")}</span>
          <TextInput mono className="kd-fname" list={dl} defaultValue={k} key={"k" + k} onBlur={(e) => e.target.value.trim() !== k && setAt(i, [e.target.value.trim(), v])}
            aria-label={t("studio.data.where.fieldAria")} data-testid={testId + "-w" + i + "-field"} placeholder={t("studio.data.where.fieldPh")} />
          <span className="kst-dim">=</span>
          <LooseBox value={v} onChange={(nv) => setAt(i, [k, nv])} label={t("studio.data.where.valueAria", { field: k })} testId={testId + "-w" + i + "-value"} />
          <IconButton label={t("studio.data.where.remove")} icon={<X size={15} />} size="s" onClick={() => onChange(where.filter((_, j) => j !== i))} data-testid={testId + "-w" + i + "-remove"} />
        </div>
      ))}
      <Button size="s" variant="ghost" onClick={() => onChange([...where, ["", ""]])} data-testid={testId + "-addwhere"}>{t("studio.data.where.add")}</Button>
    </div>
  );
}
