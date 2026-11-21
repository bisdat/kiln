// The recipe editor's "Any of" ingredient (inputs only): a picker over the game's recipe input categories, each worded
// by its items ("Any of: Wood Logs, Hardwood, Palm Wood Logs, +3 more") and found by any member item; the chosen one as a
// chip whose items (and the game's recipes that take it) show on hover and on expand. Data: studio_categories.
import { ChevronDown, ChevronUp } from "lucide-react";
import { useId, useState, type KeyboardEvent } from "react";
import { useT } from "../../i18n";
import { IconButton } from "../../ui";
import { type Category, filterCategories, findCategory, itemLabel, shownItems, summary } from "./categories";
import { ItemIcon } from "./Picker";
import { useCategories, useShowHidden } from "./store";

/** The words of a category: "Any of: A, B, C, +2 more". */
export function useAnyOf(): (c: Category) => string {
  const t = useT();
  const showHidden = useShowHidden((s) => s.on);
  return (c) => {
    const s = summary(c, showHidden);
    return s.more ? t("studio.data.recipes.anyOfMore", { names: s.names.join(", "), count: s.more }) : t("studio.data.recipes.anyOf", { names: s.names.join(", ") });
  };
}

/** A few member icons, overlapping. */
function Icons({ c, max = 3 }: { c: Category; max?: number }) {
  const showHidden = useShowHidden((s) => s.on);
  return <span className="kd-caticons" aria-hidden="true">{shownItems(c, showHidden).slice(0, max).map((i) => <ItemIcon key={i.name} name={i.name} size={22} />)}</span>;
}

/** Type to search by any item of a category; arrows, Enter, Escape as the item pickers. */
export function CategoryPicker({ onChange, testId }: { onChange: (id: string) => void; testId: string }) {
  const t = useT();
  const cats = useCategories();
  const showHidden = useShowHidden((s) => s.on);
  const anyOf = useAnyOf();
  const [text, setText] = useState("");
  const [open, setOpen] = useState(true);
  const [at, setAt] = useState(0);
  const [nav, setNav] = useState(false);
  const list = useId();
  const rows = filterCategories(cats.data ?? [], text, showHidden);
  const choose = (c: Category) => { setOpen(false); onChange(c.id); };
  const key = (e: KeyboardEvent) => {
    if (e.key === "ArrowDown") { e.preventDefault(); setOpen(true); setNav(true); setAt((a) => (nav ? Math.min(a + 1, rows.length - 1) : 0)); }
    else if (e.key === "ArrowUp") { e.preventDefault(); setNav(true); setAt((a) => Math.max(a - 1, 0)); }
    else if (e.key === "Enter") { e.preventDefault(); const c = rows[nav ? at : 0]; if (open && c && (nav || text.trim())) choose(c); }
    else if (e.key === "Escape") { setOpen(false); }
  };
  return (
    <div className="kd-pick" data-testid={testId}>
      <input className="k-input kd-pick-input kd-cat-input" value={text} placeholder={t("studio.data.recipes.categoryPh")} autoFocus
        role="combobox" aria-expanded={open} aria-controls={list} aria-autocomplete="list" aria-label={t("studio.data.recipes.categoryAria")} spellCheck={false}
        aria-activedescendant={open && nav && rows[at] ? list + "-" + at : undefined}
        onChange={(e) => { setText(e.target.value); setOpen(true); setAt(0); setNav(false); }} onFocus={() => setOpen(true)} onBlur={() => setOpen(false)} onKeyDown={key}
        data-testid={testId + "-input"} />
      {open && (
        <ul className="kd-pick-list kd-cat-list" id={list} role="listbox" aria-label={t("studio.data.recipes.categoryAria")} data-testid={testId + "-list"}>
          {rows.length === 0 && <li className="kd-pick-none">{cats.isFetching ? t("studio.data.pick.searching") : text.trim() ? t("studio.data.recipes.categoryNone") : t("studio.data.pick.none")}</li>}
          {rows.map((c, i) => (
            <li key={c.id} id={list + "-" + i} role="option" aria-selected={nav && i === at}>
              <button type="button" className={"kd-hit" + (nav && i === at ? " on" : "")} onMouseDown={(e) => e.preventDefault()} onClick={() => choose(c)}
                title={summary(c, showHidden, Infinity).all.join(", ")} data-testid={testId + "-hit-" + c.id}>
                <Icons c={c} />
                <span className="kd-hit-text">
                  <span className="kd-hit-name">{anyOf(c)}</span>
                  <span className="kd-hit-id">
                    {t("studio.data.recipes.categoryItems", { count: shownItems(c, showHidden).length })}
                    {" · "}
                    {c.recipes.length ? t("studio.data.recipes.categoryUsed", { count: c.recipes.length }) : t("studio.data.recipes.categoryUnused")}
                  </span>
                </span>
              </button>
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}

/** A chosen category: a chip in words (all its items on hover), and the expand button. */
export function CategoryChip({ id, open, onToggle, testId }: { id: string | number; open: boolean; onToggle: () => void; testId: string }) {
  const t = useT();
  const cats = useCategories();
  const showHidden = useShowHidden((s) => s.on);
  const anyOf = useAnyOf();
  const c = findCategory(cats.data ?? [], id);
  if (!c || !shownItems(c, showHidden).length) {
    return <span className="kst-mono kd-cat" data-testid={testId + "-chip"}>{t("studio.data.recipes.category", { id: String(id) })}</span>;
  }
  return (
    <span className="kd-catchip" title={summary(c, showHidden, Infinity).all.join(", ")} data-testid={testId + "-chip"}>
      <Icons c={c} />
      <span className="kd-catchip-words">{anyOf(c)}</span>
      <IconButton size="s" label={open ? t("studio.data.recipes.categoryHide") : t("studio.data.recipes.categoryShow")} aria-expanded={open}
        icon={open ? <ChevronUp size={15} /> : <ChevronDown size={15} />} onClick={onToggle} data-cat-expand="" data-testid={testId + "-expand"} />
    </span>
  );
}

/** A chosen category's items and the game's recipes that take it (under its row). */
export function CategoryPanel({ id, testId }: { id: string | number; testId: string }) {
  const t = useT();
  const cats = useCategories();
  const showHidden = useShowHidden((s) => s.on);
  const c = findCategory(cats.data ?? [], id);
  if (!c) return null;
  const recipes = [...new Set(c.recipes.map((r) => r.displayName || r.name))];
  const shown = recipes.slice(0, 8);
  return (
    <div className="kd-catpanel" data-testid={testId + "-panel"}>
      <ul className="kd-catitems" aria-label={t("studio.data.recipes.categoryAria")}>
        {shownItems(c, showHidden).map((i) => (
          <li key={i.name} className="kd-catitem">
            <ItemIcon name={i.name} size={22} />
            <span>{itemLabel(i)}</span>
            <span className="kd-hit-id">{i.name}</span>
            {i.hidden && <span className="kd-hidden-tag" title={t("studio.data.hidden.tip")}>{t("studio.data.hidden.tag")}</span>}
          </li>
        ))}
      </ul>
      <p className="kst-dim kd-note" data-testid={testId + "-recipes"}>
        {!recipes.length ? t("studio.data.recipes.categoryUnused")
          : recipes.length > shown.length ? t("studio.data.recipes.categoryUsedByMore", { names: shown.join(", "), count: recipes.length - shown.length })
            : t("studio.data.recipes.categoryUsedBy", { names: shown.join(", ") })}
      </p>
    </div>
  );
}
