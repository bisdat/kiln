// /settings (mockup A, "Section list"): the title, a search box (Ctrl+F; Esc clears it) and six sections down the left,
// each with a live line (pages/settings/summary.ts) and the open section's groups as links under it; the launcher's
// version at the bottom. The right side shows one section at a time (pages/settings/sections/*), or the search's hits as
// working controls under their place, with "Open in <section>" and the hits per section counted in the list.
// Routes: /settings opens General (or the section last shown in this run), /settings/<section> that section. A hash is
// a group's anchor (pages/settings/model.ts: #profiles, #crucible, #library, #nexus, #graphics, #upscalers,
// #shroudforge, ...): it opens the group's section, scrolls to it and flushes it with ember; a search's Enter or "Open in"
// does the same for the row it found.
import { useQuery } from "@tanstack/react-query";
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { Link, useLocation, useNavigate, useParams } from "react-router-dom";
import * as api from "../api/commands";
import { qk, useAppInfo, useDistribution, useGame, useGameSel, useNexusEnabled, useSettings } from "../api/hooks";
import { useCrucibleOn } from "../crucible/state";
import { useLanguage, useT } from "../i18n";
import { Kbd, Page, Select, WellInput } from "../ui";
import { SECTIONS, bestHit, countBySection, groupById, queryWords, resolveAnchor, search, sectionFor, sectionTitle, type SectionId, type SettingsCtx } from "./settings/model";
import { SettingsViewProvider, type SettingsView } from "./settings/parts";
import { buildIndex } from "./settings/searchIndex";
import { useSummaries } from "./settings/summary";
import { AdvancedSection } from "./settings/sections/Advanced";
import { GameSection } from "./settings/sections/GameKiln";
import { GeneralSection } from "./settings/sections/General";
import { GraphicsSection } from "./settings/sections/Graphics";
import { ModsSection } from "./settings/sections/Mods";
import { OnlineSection } from "./settings/sections/Online";
import "./settings/settings.css";

/** The section last shown in this run (sessionStorage: a new start opens General). */
const LAST = "kiln.settings.section";
const lastSection = () => { try { return sessionStorage.getItem(LAST); } catch { return null; } };
/** How long a row or group found by a jump stays flushed with ember. */
const FLUSH_MS = 2000;

const BODIES: Record<SectionId, () => JSX.Element> = {
  general: GeneralSection, mods: ModsSection, game: GameSection, graphics: GraphicsSection, online: OnlineSection, advanced: AdvancedSection,
};

/** What decides which groups and rows exist, and what search reads. */
function useSettingsCtx(): SettingsCtx {
  const info = useAppInfo();
  const settings = useSettings();
  const dist = useDistribution();
  const nexus = useNexusEnabled();
  const crucible = useCrucibleOn();
  const { game } = useGame();
  const doc = useQuery({ queryKey: qk.kilnJson(game?.id ?? ""), queryFn: () => api.configKilnRead(game!.id), enabled: !!game });
  return useMemo(() => ({
    info: info.data, settings: settings.data, dist, nexus, crucible, tauri: api.isTauri(), client: game?.kind === "client",
    schema: doc.data?.schema, values: doc.data?.values,
  }), [info.data, settings.data, dist, nexus, crucible, game?.kind, doc.data]);
}

export default function SettingsPage() {
  const t = useT();
  const lang = useLanguage();
  const nav = useNavigate();
  const loc = useLocation();
  const { section: param } = useParams();
  const section = sectionFor(param, loc.hash, lastSection());
  useEffect(() => { try { sessionStorage.setItem(LAST, section); } catch { /* private mode */ } }, [section]);

  const ctx = useSettingsCtx();
  // the index in the launcher's language: built again when the language changes
  const items = useMemo(() => buildIndex(ctx), [ctx, lang]); // eslint-disable-line react-hooks/exhaustive-deps
  const byId = useMemo(() => new Map(items.map((i) => [i.id, i])), [items]);
  const [q, setQ] = useState("");
  const words = useMemo(() => queryWords(q), [q]);
  const hits = useMemo(() => search(items, q), [items, q]);
  const counts = useMemo(() => countBySection(hits), [hits]);
  const searching = words.length > 0;
  const sums = useSummaries(ctx.values);
  const info = useAppInfo();

  const main = useRef<HTMLDivElement>(null);
  const box = useRef<HTMLInputElement>(null);
  // a jump: the row or group to find, scroll to and flush (n: a new jump to the same target flushes again)
  const [flash, setFlash] = useState<{ target: string; n: number } | null>(null);
  const jump = useCallback((s: SectionId, target: string) => {
    setQ("");
    if (s !== section || loc.hash) nav(`/settings/${s}`);
    setFlash((f) => ({ target, n: (f?.n ?? 0) + 1 }));
  }, [section, loc.hash, nav]);

  // a deep link (a hash): its section is open (sectionFor); its group is found and flushed
  useEffect(() => {
    const a = loc.hash.length > 1 ? resolveAnchor(loc.hash.slice(1)) : null;
    if (!a) return;
    setQ("");
    setFlash((f) => ({ target: a.group ?? "", n: (f?.n ?? 0) + 1 }));
  }, [loc.hash, loc.key]);

  // a new section starts at its top
  useLayoutEffect(() => { if (main.current) main.current.scrollTop = 0; }, [section, searching]);

  // find the jump's target (it may still be loading), scroll to it, flush it
  useEffect(() => {
    if (!flash || searching) return;
    const root = main.current;
    if (!root) return;
    let tries = 0, done = false;
    let timer = 0, unflush = 0;
    const find = () => {
      if (!flash.target) return root.querySelector<HTMLElement>(".k-st-head");
      // a kiln.json entry: SchemaForm's row of the control sf-<path>
      if (flash.target.startsWith("kj:")) return root.querySelector<HTMLElement>(`#sf-${flash.target.slice(3).replace(/[^a-z0-9]/gi, "-")}`)?.closest<HTMLElement>(".k-sform-row") ?? null;
      const id = CSS.escape(flash.target);
      return root.querySelector<HTMLElement>(`[data-setting="${id}"]`) ?? root.querySelector<HTMLElement>(`[data-group="${id}"]`);
    };
    const place = (el: HTMLElement) => {
      const r = el.getBoundingClientRect(), box = root.getBoundingClientRect();
      const row = !!el.dataset.setting || el.classList.contains("k-sform-row");
      // a row: in the middle when it fits; a group: its head at the top
      const off = row && r.height < box.height * 0.6 ? (box.height - r.height) / 2 : 16;
      root.scrollTop += r.top - box.top - off;
    };
    const go = () => {
      if (done) return;
      const el = find();
      if (!el) { if (tries++ < 60) timer = window.setTimeout(go, 50); return; }
      done = true;
      place(el);
      if (!flash.target) return;
      el.classList.remove("k-st-flush");
      void el.offsetWidth;
      el.classList.add("k-st-flush");
      // what loads above it can push it down: place it once more
      timer = window.setTimeout(() => place(el), 350);
      unflush = window.setTimeout(() => el.classList.remove("k-st-flush"), FLUSH_MS);
    };
    go();
    return () => { done = true; clearTimeout(timer); clearTimeout(unflush); };
  }, [flash, searching, section]);

  // Ctrl+F (Cmd+F) finds a setting while Settings is open
  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey && e.key.toLowerCase() === "f") {
        e.preventDefault();
        box.current?.focus();
        box.current?.select();
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, []);

  // the open section's groups as they are on the page, and the one at the top while it scrolls
  const [groups, setGroups] = useState<string[]>([]);
  const [current, setCurrent] = useState<string | null>(null);
  useLayoutEffect(() => {
    const root = main.current;
    if (!root || searching) { setGroups([]); return; }
    const read = () => {
      const ids = [...root.querySelectorAll<HTMLElement>("[data-group]")].map((el) => el.dataset.group!).filter((id, i, all) => all.indexOf(id) === i);
      setGroups((old) => (old.join() === ids.join() ? old : ids));
    };
    read();
    const mo = new MutationObserver(read);
    mo.observe(root, { childList: true, subtree: true });
    return () => mo.disconnect();
  }, [section, searching]);
  useEffect(() => {
    const root = main.current;
    if (!root || searching) return;
    const spy = () => {
      const top = root.getBoundingClientRect().top + 48;
      let at: string | null = null;
      for (const el of root.querySelectorAll<HTMLElement>("[data-group]")) if (el.getBoundingClientRect().top <= top) at = el.dataset.group!;
      // at the very end, the last group is the one being read
      if (root.scrollHeight > root.clientHeight + 2 && root.scrollTop + root.clientHeight >= root.scrollHeight - 2 && groups.length) at = groups[groups.length - 1];
      setCurrent(at ?? groups[0] ?? null);
    };
    spy();
    root.addEventListener("scroll", spy, { passive: true });
    return () => root.removeEventListener("scroll", spy);
  }, [groups, searching]);
  const toGroup = (id: string) => {
    const root = main.current;
    const el = root?.querySelector<HTMLElement>(`[data-group="${CSS.escape(id)}"]`);
    if (!root || !el) return;
    root.scrollTo({ top: root.scrollTop + el.getBoundingClientRect().top - root.getBoundingClientRect().top - 16, behavior: "smooth" });
  };

  const view: SettingsView = useMemo(() => ({ ctx, hits: searching ? new Set(hits.map((h) => h.id)) : null, words, items: byId, jump }),
    [ctx, searching, hits, words, byId, jump]);
  const Body = BODIES[section];
  const hitSections = SECTIONS.filter((s) => counts[s]);
  const say = !searching ? sums.say[section]
    : hits.length ? [t("settings.search.found", { count: hits.length, query: q.trim() }), hitSections.length === 1 && t("settings.search.allIn", { section: sectionTitle(hitSections[0]) }),
      t("settings.search.change", { count: hits.length })].filter(Boolean).join(" ")
    : t("settings.search.none", { query: q.trim() });

  return (
    <Page fill className="k-settings">
      <SettingsViewProvider value={view}>
        <div className="k-st">
          <aside className="k-st-rail">
            <h1 className="k-page-title k-st-pagename">{t("settings.title")}</h1>
            <WellInput ref={box} label={t("settings.search.label")} value={q} compact
              onChange={(e) => setQ(e.target.value)} onClear={() => setQ("")} clearLabel={t("settings.search.clear")}
              note={q ? undefined : <Kbd>{t("settings.search.key")}</Kbd>}
              onKeyDown={(e) => { const best = e.key === "Enter" ? bestHit(hits, q) : null; if (best) { e.preventDefault(); jump(best.section, best.id); } }} />
            <nav className="k-st-nav" aria-label={t("settings.nav.label")}>
              <ul>
                {SECTIONS.map((s) => {
                  const on = !searching && s === section;
                  const n = counts[s] ?? 0;
                  const line = searching ? (n ? [...new Set(hits.filter((h) => h.section === s).map((h) => groupById(h.group)?.title() ?? ""))].join(", ") : "") : sums.line[s];
                  return (
                    <li key={s} className={["k-st-sec", on && "on", searching && !n && "dim", s === "advanced" && "last"].filter(Boolean).join(" ")}>
                      <Link to={`/settings/${s}`} aria-current={on ? "page" : undefined} onClick={() => setQ("")} data-section={s}
                        aria-label={searching && n ? t("settings.nav.hits", { section: sectionTitle(s), count: n }) : sectionTitle(s)}>
                        <span className="k-st-sec-name">{sectionTitle(s)}</span>
                        {line && <span className="k-st-sec-line">{line}</span>}
                        {searching && n > 0 && <span className="k-st-count" aria-hidden>{n}</span>}
                      </Link>
                      {on && groups.length > 1 && (
                        <ul className="k-st-subs" aria-label={t("settings.nav.groups", { section: sectionTitle(s) })}>
                          {groups.map((g) => (
                            <li key={g}>
                              <button type="button" className={g === current ? "on" : undefined} aria-current={g === current ? "true" : undefined}
                                onClick={() => toGroup(g)}>{groupById(g)?.title()}</button>
                            </li>
                          ))}
                        </ul>
                      )}
                    </li>
                  );
                })}
              </ul>
            </nav>
            <p className="k-st-foot" title={t("settings.version", { version: info.data?.version ?? "" })}><span className="k-mono">{info.data?.version ?? ""}</span></p>
          </aside>
          <div className="k-st-main" ref={main}>
            <header className="k-st-head">
              <div className="k-st-head-text">
                <h2 className="k-st-title">{searching ? t("settings.search.title") : sectionTitle(section)}</h2>
                {say && <p className="k-st-say" role={searching ? "status" : undefined}>{say}</p>}
              </div>
              {!searching && (section === "game" || section === "graphics") && <GamePicker />}
            </header>
            <div className="k-st-body">
              {searching ? (
                <>
                  {hitSections.map((s) => { const B = BODIES[s]; return <B key={s} />; })}
                  <p className="k-st-searchfoot">{t("settings.search.foot")}</p>
                </>
              ) : <Body />}
            </div>
          </div>
        </div>
      </SettingsViewProvider>
    </Page>
  );
}

/** "For <game>": which game the per-game sections show (the same choice as Play's game). */
function GamePicker() {
  const t = useT();
  const { game, games } = useGame();
  const setGameId = useGameSel((s) => s.setGameId);
  const clients = games.filter((g) => g.kind === "client");
  if (!game || clients.length < 2) return null;
  return (
    <label className="k-st-for">
      <span className="k-sc">{t("settings.for")}</span>
      <Select aria-label={t("play.game")} value={game.id} onChange={setGameId} style={{ width: 220 }}
        options={clients.map((g) => ({ value: g.id, label: g.path.split(/[\\/]/).pop() ?? g.id }))} />
    </label>
  );
}
