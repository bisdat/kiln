// Pack Studio's shared state: the queries of a pack source and the save that follows the creator's edits.
import { useMutation, useQuery, useQueryClient, type QueryClient } from "@tanstack/react-query";
import { useEffect, useRef } from "react";
import { create } from "zustand";
import * as api from "../api/commands";
import type { PackCheckReport, PackSourceDoc, Studio } from "../api/commands";
import { runTask } from "../ui";

export const sk = {
  list: ["studio_list"] as const,
  get: (id: string) => ["studio_get", id] as const,
  capture: (id: string) => ["studio_capture", id] as const,
  order: (id: string) => ["studio_order", id] as const,
  layer: (id: string) => ["studio_layer_files", id] as const,
  lint: (id: string) => ["studio_layer_lint", id] as const,
  catalog: ["studio_catalog_status"] as const,
};

/** The game-data catalog's upkeep (kiln_manage::catalog::auto), polled while Kiln reads the game's items. */
export const useStudioCatalog = () => useQuery({
  queryKey: sk.catalog, queryFn: api.studioCatalogStatus, retry: false,
  refetchInterval: (q) => (q.state.data?.state === "building" ? 400 : false),
});

/** Pack Studio opening and "Build the game catalog": Kiln reads the game's items in the background when its catalog is
 *  missing or not current. The answer ("building") replaces a status read that was still on its way. */
export async function ensureCatalog(qc: QueryClient) {
  try {
    const s = await api.studioCatalogEnsure();
    await qc.cancelQueries({ queryKey: sk.catalog });
    qc.setQueryData(sk.catalog, s);
  } catch { /* the Data tab says what is missing */ }
}

/** One check when Pack Studio opens; once Kiln made a catalog, the pickers, fields and lint read again. */
export function useCatalogUpkeep() {
  const qc = useQueryClient();
  const cat = useStudioCatalog();
  useEffect(() => { void ensureCatalog(qc); }, [qc]);
  const prev = useRef(cat.data?.state);
  useEffect(() => {
    const s = cat.data;
    if (prev.current === "building" && s?.state === "ready" && s.built) {
      for (const k of [["studio_pick"], ["studio_categories"], ["studio_fields"], ["studio_layer_lint"]]) void qc.invalidateQueries({ queryKey: k });
    }
    prev.current = s?.state;
  }, [cat.data, qc]);
}

export const useStudioList = () => useQuery({ queryKey: sk.list, queryFn: api.studioList, retry: false });
export const useStudio = (id: string | undefined) => useQuery({ queryKey: sk.get(id ?? ""), queryFn: () => api.studioGet(id!), enabled: !!id, retry: false });

/** Everything that reads a pack source (after a change to it or its profile). */
export function refreshStudio(qc: QueryClient, id: string) {
  for (const k of [sk.list, sk.get(id), sk.capture(id), sk.order(id), sk.layer(id), sk.lint(id), ["profile_list"], ["profile_get"], ["profile_resolve"], ["library_list"]]) void qc.invalidateQueries({ queryKey: k });
}

/** The source as the cache has it now (with edits not answered yet): a second quick change builds on the first. */
export function latestSource(qc: QueryClient, id: string, fallback: PackSourceDoc): PackSourceDoc {
  return qc.getQueryData<Studio>(sk.get(id))?.source ?? fallback;
}

/** Saves a changed source: the cache shows it at once, the answer replaces it; one save at a time per pack. */
export function useSaveSource(id: string) {
  const qc = useQueryClient();
  return useMutation({
    mutationKey: ["studio_save", id],
    scope: { id: "studio_save:" + id },
    onMutate: (source: PackSourceDoc) => {
      const cur = qc.getQueryData<Studio>(sk.get(id));
      if (cur) qc.setQueryData(sk.get(id), { ...cur, source });
      return { cur };
    },
    mutationFn: (source: PackSourceDoc) => api.studioSave(id, source),
    onSuccess: (st: Studio) => { qc.setQueryData(sk.get(id), st); void qc.invalidateQueries({ queryKey: sk.list }); },
    onError: () => { void qc.invalidateQueries({ queryKey: sk.get(id) }); },
  });
}

export const TABS = ["mods", "settings", "data", "order", "test", "credits", "publish"] as const;
export type StudioTab = (typeof TABS)[number];

// ---------------------------------------------------------------- the analysis (checks, conflicts, prefilled credits)

interface Analysis { report: PackCheckReport | null; stamp: string | null; running: boolean; error: string | null }
const EMPTY: Analysis = { report: null, stamp: null, running: false, error: null };
/** The last analysis per pack and the source's updated time it was made for. */
export const useAnalysis = create<{ by: Record<string, Analysis>; set: (id: string, a: Partial<Analysis>) => void }>((set) => ({
  by: {},
  set: (id, a) => set((s) => ({ by: { ...s.by, [id]: { ...EMPTY, ...s.by[id], ...a } } })),
}));

/** Runs the analysis (a task) unless one runs; keeps it for the tabs. */
export async function analyze(id: string, stamp: string): Promise<PackCheckReport | null> {
  const cur = useAnalysis.getState().by[id];
  if (cur?.running) return null;
  useAnalysis.getState().set(id, { running: true, error: null });
  try {
    const report = await runTask<PackCheckReport>(() => api.studioAnalyze(id), { quiet: true });
    useAnalysis.getState().set(id, { report, stamp, running: false });
    return report;
  } catch (e) {
    useAnalysis.getState().set(id, { running: false, error: api.toKilnError(e).message });
    return null;
  }
}

/** The analysis of the pack as it is (made again when the source changed since). */
export function useAnalysisOf(st: Studio | undefined, auto = true): Analysis & { stale: boolean } {
  const a = useAnalysis((s) => (st ? s.by[st.source.id] : undefined)) ?? EMPTY;
  const stale = !!st && a.stamp !== st.revision;
  if (auto && st && stale && !a.running && !a.error) queueMicrotask(() => void analyze(st.source.id, st.revision));
  return { ...a, stale };
}

// ---------------------------------------------------------------- words of the analysis

/** How a credit was found (CreditRow.match) as words. */
export const MATCH_WORD: Record<string, string> = {
  kiln: "studio.match.kiln", crucible: "studio.match.crucible", github: "studio.match.github", nexus: "studio.match.nexus", manifest: "studio.match.manifest", manual: "studio.match.manual",
};
export const matchWord = (m: string) => MATCH_WORD[m] ?? MATCH_WORD.manual;
/** How sure the identification is (CreditRow.confidence). */
const CONFIDENCE_WORD: Record<string, string> = {
  certain: "studio.confidence.certain", likely: "studio.confidence.likely", conflict: "studio.confidence.conflict", unverified: "studio.confidence.unverified",
  creator: "studio.confidence.creator", unconfirmed: "studio.confidence.unconfirmed", none: "studio.confidence.none",
};
export const confidenceWord = (c: string) => CONFIDENCE_WORD[c] ?? CONFIDENCE_WORD.unconfirmed;
/** A check's state word. */
const TONE_WORD: Record<string, string> = { ok: "studio.publish.tone.ok", warn: "studio.publish.tone.warn", err: "studio.publish.tone.err", note: "studio.publish.tone.note" };
export const toneWord = (x: string) => TONE_WORD[x] ?? TONE_WORD.note;
