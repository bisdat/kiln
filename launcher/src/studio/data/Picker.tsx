// The pickers: game entries (the catalog) and what the pack's mods and the layer add, with the game's item icons
// (gameart: <art>/icons/<name>_icon.webp, mod items from the overlay <art>/modded/items.json; a neutral square when
// the art is missing, as the save editor shows).
import { createContext, useContext, useEffect, useId, useRef, useState, type KeyboardEvent } from "react";
import { isTauri, type StudioPickHit } from "../../api/commands";
import { STATION_WATER } from "../../api/hiddenNames";
import { useT } from "../../i18n";
import { useEntry, usePick } from "./store";

export interface DataCtxValue {
  packId: string;
  /** A mod's name by id ("pack.<id>": this pack). */
  fromName: (from: string | undefined) => string | null;
  /** New items of the layer: name -> base item (their icon is the base's). */
  bases: Record<string, string>;
}
export const DataCtx = createContext<DataCtxValue>({ packId: "", fromName: () => null, bases: {} });
export const useDataCtx = () => useContext(DataCtx);

// the editor's asset base (src/editor/src/platform.js; not imported: it pulls the save editor's tables)
const WINDOWS = typeof navigator !== "undefined" && /Windows/i.test(navigator.userAgent);
const TAURI = isTauri();
export const ASSET_BASE = import.meta.env.DEV || !TAURI ? "/assets-local/editor/" : WINDOWS ? "http://kiln-asset.localhost/editor/" : "kiln-asset://localhost/editor/";

/** The save editor's neutral square (src/editor/kiln/parts.jsx PLACEHOLDER). */
export const PLACEHOLDER = "data:image/svg+xml," + encodeURIComponent(
  '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect x="4" y="4" width="56" height="56" rx="8" fill="#2a2421" stroke="#4d413a" stroke-width="2"/><path d="M22 42l8-10 6 7 4-5 6 8z" fill="#6b5e55"/><circle cx="26" cy="24" r="4" fill="#6b5e55"/></svg>');

// the mod overlay: debug name -> icon path (loaded once)
let overlay: Promise<Record<string, string>> | null = null;
function modIcons(): Promise<Record<string, string>> {
  overlay ??= fetch(ASSET_BASE + "modded/items.json").then((r) => (r.ok && (r.headers.get("content-type") ?? "").includes("json") ? r.json() : null)).then((o) => {
    const out: Record<string, string> = {};
    for (const m of (o?.items ?? []) as { debug?: string; icon?: string | null }[]) if (m.debug && m.icon) out[m.debug] = m.icon;
    return out;
  }).catch(() => ({}));
  return overlay;
}

/** An item's icon by its debug name (a new item of the layer: its base's). */
export function ItemIcon({ name, size = 28 }: { name: string; size?: number }) {
  const { bases } = useDataCtx();
  const [mods, setMods] = useState<Record<string, string> | null>(null);
  const [broken, setBroken] = useState(false);
  useEffect(() => { let live = true; void modIcons().then((m) => { if (live) setMods(m); }); return () => { live = false; }; }, []);
  useEffect(() => setBroken(false), [name, mods]);
  const n = bases[name] ?? name;
  const ok = !!n && /^[A-Za-z0-9_.-]+$/.test(n);
  const src = broken || !ok || mods === null ? PLACEHOLDER : mods[n] ? ASSET_BASE + mods[n] : ASSET_BASE + "icons/" + n + "_icon.webp";
  return <img className="kd-icon" src={src} alt="" width={size} height={size} onError={() => setBroken(true)} draggable={false} />;
}

/** "from Kiln Equipment" / "this pack" for a hit. */
function FromBadge({ from }: { from?: string }) {
  const { fromName } = useDataCtx();
  const n = fromName(from);
  return n ? <span className="kd-from">{n}</span> : null;
}

export interface PickerProps {
  section: string;
  value: string;
  onChange: (name: string, hit?: StudioPickHit) => void;
  /** Globs and free names are taken as typed (selectors). */
  free?: boolean;
  /** Item icons (items sections). */
  icons?: boolean;
  /** What a hit's chosen value is (drop tables: the GUID). */
  pickValue?: (h: StudioPickHit) => string;
  placeholder?: string;
  "aria-label"?: string;
  testId?: string;
}

/** A name box with a list of entries under it: type to search, click or Enter to choose. */
export function Picker({ section, value, onChange, free, icons, pickValue, placeholder, testId, ...rest }: PickerProps) {
  const t = useT();
  const { packId } = useDataCtx();
  const [text, setText] = useState(value);
  const [open, setOpen] = useState(false);
  const [at, setAt] = useState(0);
  // Enter takes the highlighted hit only after the arrows moved it; else an exact name, else the text as typed
  const [nav, setNav] = useState(false);
  const list = useId();
  const box = useRef<HTMLInputElement>(null);
  useEffect(() => setText(value), [value]);
  const hits = usePick(packId, section, open ? (text === value ? "" : text) : "", open);
  const entry = useEntry(packId, section, value);
  const rows = hits.data ?? [];
  const valueOf = (h: StudioPickHit) => (pickValue ? pickValue(h) : h.name);
  const choose = (h: StudioPickHit) => { const v = valueOf(h); setText(v); setOpen(false); onChange(v, h); };
  const commit = () => { setOpen(false); if (text.trim() !== value) onChange(text.trim()); };
  const key = (e: KeyboardEvent) => {
    if (e.key === "ArrowDown") { e.preventDefault(); setOpen(true); setNav(true); setAt((a) => (nav ? Math.min(a + 1, rows.length - 1) : 0)); }
    else if (e.key === "ArrowUp") { e.preventDefault(); setNav(true); setAt((a) => Math.max(a - 1, 0)); }
    else if (e.key === "Enter") {
      e.preventDefault();
      const exact = rows.find((h) => h.name.toLowerCase() === text.trim().toLowerCase());
      if (open && nav && rows[at]) choose(rows[at]); else if (open && exact) choose(exact); else commit();
    }
    else if (e.key === "Escape") { setText(value); setOpen(false); }
  };
  const glob = /[*?]/.test(value);
  return (
    <div className="kd-pick" data-testid={testId}>
      {icons && <ItemIcon name={value} />}
      <input ref={box} className="k-input kd-pick-input" value={text} placeholder={placeholder ?? (free ? t("studio.data.pick.selectorPh") : t("studio.data.pick.ph"))}
        role="combobox" aria-expanded={open} aria-controls={list} aria-autocomplete="list" aria-label={rest["aria-label"]} spellCheck={false}
        onChange={(e) => { setText(e.target.value); setOpen(true); setAt(0); setNav(false); }} onFocus={() => setOpen(true)} onBlur={commit} onKeyDown={key}
        data-testid={testId ? testId + "-input" : undefined} />
      {value && !open && (
        <span className="kd-pick-name" data-testid={testId ? testId + "-name" : undefined}>
          {glob ? t("studio.data.pick.glob") : entry ? (entry.displayName ?? entry.name) : free ? "" : t("studio.data.pick.unknown")}
          {entry && <FromBadge from={entry.from} />}
          {value === STATION_WATER && <span className="kd-hidden-tag" data-testid={testId ? testId + "-note" : undefined}>{t("studio.data.hidden.tankWater")}</span>}
          {entry?.hidden && <span className="kd-hidden-tag" title={t("studio.data.hidden.tip")} data-testid={testId ? testId + "-hidden" : undefined}>{t("studio.data.hidden.tag")}</span>}
        </span>
      )}
      {open && (
        <ul className="kd-pick-list" id={list} role="listbox" data-testid={testId ? testId + "-list" : undefined}>
          {rows.length === 0 && <li className="kd-pick-none">{hits.isFetching ? t("studio.data.pick.searching") : t("studio.data.pick.none")}</li>}
          {rows.map((h, i) => (
            <li key={(h.from ?? "") + h.name} role="option" aria-selected={i === at}>
              <button type="button" className={"kd-hit" + (nav && i === at ? " on" : "")} onMouseDown={(e) => e.preventDefault()} onClick={() => choose(h)}
                data-testid={testId ? testId + "-hit-" + h.name : undefined}>
                {icons && <ItemIcon name={h.name} size={26} />}
                <span className="kd-hit-text">
                  <span className="kd-hit-name">{h.displayName ?? h.name}</span>
                  <span className="kd-hit-id">{h.name}</span>
                </span>
                {h.name === STATION_WATER && <span className="kd-hidden-tag" data-testid={testId ? testId + "-hit-note" : undefined}>{t("studio.data.hidden.tankWater")}</span>}
                {h.hidden && <span className="kd-hidden-tag" title={t("studio.data.hidden.tip")}>{t("studio.data.hidden.tag")}</span>}
                <FromBadge from={h.from} />
              </button>
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
