// The pack layer's data editors (DESIGN 3.3): Items, Recipes, Loot, Drops, New items, Raw JSON, with the game's art
// icons in the pickers and the live lint (kiln lint over the layer, answered by every write). Each editor writes an
// ordinary content/*.json section of the pack layer and keeps the rest of the file as the creator wrote it.
import { useMemo, useState } from "react";
import { useQueryClient } from "@tanstack/react-query";
import type { GameId, Studio, StudioCatalogStatus } from "../../api/commands";
import { useT } from "../../i18n";
import { ensureCatalog, useStudioCatalog } from "../state";
import { Button, Spinner, Tabs } from "../../ui";
import { isObj, readDoc } from "./model";
import { DataCtx, type DataCtxValue } from "./Picker";
import RulesEditor from "./RulesEditor";
import RecipesEditor from "./RecipesEditor";
import DropsEditor from "./DropsEditor";
import NewItemsEditor from "./NewItemsEditor";
import RawEditor, { DiagList } from "./RawEditor";
import { useDrafts, useLayerTexts, useLint, useShowHidden } from "./store";
import "./data.css";

const SUBS = ["items", "recipes", "loot", "drops", "added", "raw"] as const;
type Sub = (typeof SUBS)[number];
/** Why Kiln could not read the game's items (catalog::auto's refusals), for the no-catalog note. */
function catalogWhy(t: ReturnType<typeof useT>, s: StudioCatalogStatus | undefined): string | null {
  if (!s || (s.state !== "failed" && s.state !== "noGame")) return null;
  const code = s.error?.code;
  if (s.state === "noGame" || code === "not_found") return t("studio.data.catalog.why.noGame");
  if (code === "steam_updating") return t("studio.data.catalog.why.steamUpdating");
  if (code === "conflict") return t("studio.data.catalog.why.conflict");
  return t("studio.data.catalog.why.failed", { message: s.error?.message ?? "" });
}

/** The sub-tab each pack was left on. */
const lastSub = new Map<string, Sub>();

export default function DataTab({ studio }: { studio: Studio; gameId: GameId | null }) {
  const t = useT();
  const packId = studio.source.id;
  const [sub, setSubState] = useState<Sub>(lastSub.get(packId) ?? "items");
  const setSub = (s: Sub) => { lastSub.set(packId, s); setSubState(s); };
  const texts = useLayerTexts(packId);
  const lint = useLint(packId);
  const qc = useQueryClient();
  const cat = useStudioCatalog();
  // shown while no catalog is in use (a rebuild after a game update keeps the older one until it is done)
  const building = cat.data?.state === "building" && !!lint.data && !lint.data.catalog;
  const why = catalogWhy(t, cat.data);
  const [showLint, setShowLint] = useState(false);
  const drafts = useDrafts((s) => s.by);
  const dirty = (k: string) => Object.entries(drafts).some(([key, d]) => d.dirty && (key === packId + ":" + k || (k === "raw" && key.startsWith(packId + ":raw:"))));
  const label: Record<Sub, string> = {
    items: t("studio.data.tab.items"), recipes: t("studio.data.tab.recipes"), loot: t("studio.data.tab.loot"), drops: t("studio.data.tab.drops"),
    added: t("studio.data.tab.added"), raw: t("studio.data.tab.raw"),
  };
  const ctx = useMemo<DataCtxValue>(() => {
    const bases: Record<string, string> = {};
    for (const text of Object.values(texts.data ?? {})) {
      const items = readDoc(text).value.items;
      const add = isObj(items) && Array.isArray(items.add) ? items.add : [];
      for (const a of add) if (isObj(a) && typeof a.name === "string" && typeof a.base === "string") bases[a.name] = a.base;
    }
    return {
      packId,
      bases,
      fromName: (from) => (!from ? null : from === "pack." + packId ? t("studio.data.thisPack") : studio.mods.find((m) => m.id === from)?.name ?? from),
    };
  }, [packId, texts.data, studio.mods, t]);
  const sum = lint.data?.summary;
  const showHidden = useShowHidden();
  // notes (a hidden game entry, a language no game language uses...) are counted once there are any
  const notes = (sum?.infos ?? 0) + (sum?.hints ?? 0);
  const counts = sum ? { errors: t("studio.data.lint.errors", { count: sum.errors }), warnings: t("studio.data.lint.warnings", { count: sum.warnings }) } : null;
  const lintLine = !counts ? t("studio.data.lint.checking") : notes ? t("studio.data.lint.lineNotes", { ...counts, notes: t("studio.data.lint.notes", { count: notes }) }) : t("studio.data.lint.line", counts);
  const diags = lint.data?.diagnostics ?? [];
  return (
    <DataCtx.Provider value={ctx}>
      <div className="kd" data-testid="studio-data">
        <div className="kd-head">
          <Tabs aria-label={t("studio.data.tabsAria")} value={sub} onChange={setSub}
            items={SUBS.map((id) => ({ id, label: <span data-testid={"data-sub-" + id}>{label[id]}{dirty(id) ? " •" : ""}</span> }))} />
          <span className="kd-grow" />
          <label className="kst-tick kd-hidden-toggle" title={t("studio.data.hidden.tip")}>
            <input type="checkbox" className="k-check" checked={showHidden.on} onChange={(e) => showHidden.set(e.target.checked)} data-testid="data-show-hidden" />
            <span>{t("studio.data.hidden.toggle")}</span>
          </label>
          <span className={"kd-lint" + (sum?.errors ? " kd-lint-err" : sum?.warnings ? " kd-lint-warn" : "")} data-testid="data-lint">{lintLine}</span>
          {diags.length > 0 && <Button size="s" variant="ghost" onClick={() => setShowLint(!showLint)} aria-expanded={showLint} data-testid="data-lint-toggle">{showLint ? t("studio.data.lint.hide") : t("studio.data.lint.show")}</Button>}
        </div>
        {building && <p className="kst-dim kd-note kd-catalog" data-testid="data-catalog-building"><Spinner size={12} /> {t("studio.data.catalog.reading")}</p>}
        {lint.data && !lint.data.catalog && !building && (
          <div className="kd-catalog-missing" role="status" data-testid="data-nocatalog">
            <p className="kst-dim kd-note">{t("studio.data.noCatalog")}</p>
            {why && <p className="kd-note" data-testid="data-catalog-why">{why}</p>}
            <Button size="s" onClick={() => void ensureCatalog(qc)} data-testid="data-catalog-build">{t("studio.data.catalog.build")}</Button>
          </div>
        )}
        {showLint && <DiagList diags={diags} testId="data-lint-list" />}
        {texts.isLoading ? <Spinner /> : (
          <div className="kd-body">
            {sub === "items" && <RulesEditor key="items" section="items" texts={texts.data} />}
            {sub === "recipes" && <RecipesEditor texts={texts.data} />}
            {sub === "loot" && <RulesEditor key="loot" section="loot" texts={texts.data} />}
            {sub === "drops" && <DropsEditor texts={texts.data} />}
            {sub === "added" && <NewItemsEditor texts={texts.data} />}
            {sub === "raw" && <RawEditor texts={texts.data} />}
          </div>
        )}
      </div>
    </DataCtx.Provider>
  );
}
