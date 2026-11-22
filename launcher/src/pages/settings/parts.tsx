// Settings' building blocks (mockup A): a group (a titled list of rows; in a search, its place and "Open in <section>"),
// a row (its name, at most one line of help with More for the rest, a word such as NEXT START, the control on the
// right), the marks search puts on what it found. Rows and groups read their words from the search index
// (./searchIndex.ts), so what search finds is what the row says.
import { createContext, useContext, useEffect, useId, useRef, useState, type ReactNode } from "react";
import { useT } from "../../i18n";
import { Card } from "../../ui";
import { groupById, markParts, matchItem, sectionTitle, snippet, type SearchItem, type SectionId, type SettingsCtx } from "./model";

export interface SettingsView {
  ctx: SettingsCtx;
  /** The search's hits (ids); null: a section is shown, not a search. */
  hits: Set<string> | null;
  /** The query's words (folded), for the marks. */
  words: string[];
  items: Map<string, SearchItem>;
  /** Opens a section, scrolls to a row or group and flushes it with ember. */
  jump: (section: SectionId, target: string) => void;
}

const View = createContext<SettingsView | null>(null);
export const SettingsViewProvider = View.Provider;
export function useSettingsView(): SettingsView {
  const v = useContext(View);
  if (!v) throw new Error("useSettingsView outside Settings");
  return v;
}

/** True when this item shows: always on a section, only as a hit in a search. */
export function useShown(ids: string | string[]): boolean {
  const v = useSettingsView();
  if (!v.hits) return true;
  return (Array.isArray(ids) ? ids : [ids]).some((id) => v.hits!.has(id));
}

/** Text with the query's words marked. */
export function Hi({ text }: { text: string }) {
  const { words, hits } = useSettingsView();
  if (!hits || !words.length) return <>{text}</>;
  return <>{markParts(text, words).map((p, i) => (p.hit ? <mark key={i} className="k-st-mark">{p.text}</mark> : <span key={i}>{p.text}</span>))}</>;
}

/** The More/Less switch of a row or group. */
function MoreButton({ open, onClick, controls }: { open: boolean; onClick: () => void; controls: string }) {
  const t = useT();
  return (
    <button type="button" className="k-link k-st-more" aria-expanded={open} aria-controls={controls} onClick={onClick}>
      {open ? t("settings.less") : t("settings.more")}
    </button>
  );
}

export interface SettingRowProps {
  /** The row's id (its `data-setting`, its search item). */
  id: string;
  control?: ReactNode;
  /** The control's id, for the label. */
  htmlFor?: string;
  /** A small-caps word beside the name: when it applies ("next start"), whose it is ("this game"). */
  tag?: ReactNode;
  /** In place of the index's one line (a state that says why the control waits). */
  hint?: ReactNode;
  /** In place of the index's More text (words only the page knows: the program that had the links). */
  more?: string;
  /** After the More text (the translation files' folder). */
  moreExtra?: ReactNode;
  /** The control takes the row's whole width, under the name (a list of folders, a path). */
  wide?: boolean;
  testId?: string;
}

/** The control of a row: the element its label points at, else the control's first part (a group of choices, a field,
 *  a button). */
function controlOf(row: HTMLElement, htmlFor?: string): HTMLElement | null {
  if (htmlFor) return row.querySelector<HTMLElement>(`#${CSS.escape(htmlFor)}`);
  return row.querySelector<HTMLElement>(".k-st-ctl [role=radiogroup], .k-st-ctl [role=group], .k-st-ctl input, .k-st-ctl select, .k-st-ctl textarea, .k-st-ctl button");
}

/** One setting: name, one line of help (More opens the rest under it), a tag, the control on the right. */
export function SettingRow({ id, control, htmlFor, tag, hint, more: moreOwn, moreExtra, wide, testId }: SettingRowProps) {
  const t = useT();
  const v = useSettingsView();
  const [open, setOpen] = useState(false);
  const uid = useId();
  const row = useRef<HTMLDivElement>(null);
  const item = v.items.get(id);
  const help = hint !== undefined ? hint : item?.hint;
  // the control is described by the row's tag and line of help (a screen reader reads them with it). The controls are of
  // many kinds (a switch, a list, a path picker), so the attribute goes on the element itself, keeping any it has
  const described = [tag ? `${uid}-tag` : "", help ? `${uid}-hint` : ""].filter(Boolean).join(" ");
  useEffect(() => {
    const el = row.current && controlOf(row.current, htmlFor);
    if (!el) return;
    const own = new Set([`${uid}-tag`, `${uid}-hint`]);
    const others = (el.getAttribute("aria-describedby") ?? "").split(/\s+/).filter((x) => x && !own.has(x));
    const next = [described, ...others].filter(Boolean).join(" ");
    if (next) el.setAttribute("aria-describedby", next);
    else el.removeAttribute("aria-describedby");
  });
  if (!item) return null;
  if (v.hits && !v.hits.has(id)) return null;
  const more = moreOwn ?? item.more;
  const where = v.hits ? matchItem(item, v.words) : null;
  const quote = where?.in === "more" && more && !open ? snippet(more, v.words) : null;
  const Label = htmlFor ? "label" : "span";
  return (
    <div className={"k-st-row" + (wide ? " wide" : "")} data-setting={id} data-testid={testId} ref={row}>
      <div className="k-st-meta">
        <div className="k-st-lab">
          <Label className="k-st-name" htmlFor={htmlFor} id={`${uid}-name`}><Hi text={item.label} /></Label>
          {tag && <span className="k-st-when" id={`${uid}-tag`}>{tag}</span>}
        </div>
        {help || more ? (
          <div className="k-st-hint">
            {help ? <span id={`${uid}-hint`}>{hint !== undefined ? hint : <Hi text={item.hint!} />}</span> : null}
            {more && <MoreButton open={open} onClick={() => setOpen(!open)} controls={`${uid}-more`} />}
          </div>
        ) : null}
        {quote && <div className="k-st-hint k-st-quote">{t("settings.search.inMore")} “<Hi text={quote} />”</div>}
      </div>
      {control !== undefined && <div className="k-st-ctl">{control}</div>}
      {more && open && (
        <div className="k-st-mtext" id={`${uid}-more`}>
          <Hi text={more} />
          {moreExtra && <div className="k-st-mextra">{moreExtra}</div>}
        </div>
      )}
    </div>
  );
}

/** A search's head for a group: where it lives, and the link that opens it there. */
function Crumb({ section, title, target }: { section: SectionId; title?: string; target: string }) {
  const t = useT();
  const v = useSettingsView();
  const name = sectionTitle(section);
  return (
    <span className="k-st-crumb">
      <span>{name} ›{title !== undefined && <> <b>{title}</b></>}</span>
      <button type="button" className="k-link" onClick={() => v.jump(section, target)}>{t("settings.search.openIn", { section: name })}</button>
    </span>
  );
}

export interface GroupProps {
  id: string;
  /** The search items in it (rows, kiln.json entries); the group shows in a search when one of them is a hit. */
  items: string[];
  hint?: ReactNode;
  /** The group's longer text, behind More beside its hint (a string is marked by a search). */
  more?: ReactNode;
  actions?: ReactNode;
  testId?: string;
  children: ReactNode;
}

/** A titled group of rows. Its id is its anchor; in a search it says where it lives and links there. */
export function Group({ id, items, hint, more, actions, testId, children }: GroupProps) {
  const v = useSettingsView();
  const [open, setOpen] = useState(false);
  const uid = useId();
  const def = groupById(id);
  if (!def || (def.when && !def.when(v.ctx))) return null;
  const hits = v.hits ? items.filter((i) => v.hits!.has(i)) : null;
  if (hits && !hits.length) return null;
  const title = def.title();
  const sub = hint || more ? (
    <span className="k-st-ghint">
      {hint}
      {more && <MoreButton open={open} onClick={() => setOpen(!open)} controls={`${uid}-more`} />}
    </span>
  ) : undefined;
  return (
    <Card className="k-st-group" data-group={id} data-testid={testId} aria-label={title} sub={sub} actions={actions}
      title={hits ? <Crumb section={def.section} title={title} target={hits[0]} /> : title}>
      {more && open && <div className="k-st-mtext" id={`${uid}-more`}>{typeof more === "string" ? <Hi text={more} /> : more}</div>}
      {children}
    </Card>
  );
}

/** A group that is a whole card of its own (Profiles, Library, Nexus Mods, ShroudForge, Updates): in a search, it shows
 *  when its card matched, under the place it lives. */
export function CardGroup({ id, items, children }: { id: string; items?: string[]; children: ReactNode }) {
  const v = useSettingsView();
  const def = groupById(id);
  // a group this game or build does not have (ShroudForge without a game client): no card, no link in the list
  if (!def || (def.when && !def.when(v.ctx))) return null;
  if (v.hits && !(items ?? [id]).some((i) => v.hits!.has(i))) return null;
  return (
    <div className="k-st-cardgroup" data-group={id}>
      {v.hits && <Crumb section={def.section} target={id} />}
      {children}
    </div>
  );
}
