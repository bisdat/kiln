// The data editors' shared state: the layer's files (texts), the live lint, writes, the editors' unsaved drafts and the
// pickers' lookups (game entries and what the pack's mods add, studio_pick).
import { useQuery, useQueryClient, type QueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import { create } from "zustand";
import * as api from "../../api/commands";
import type { LintReport, StudioPickHit } from "../../api/commands";
import { sk } from "../state";

export const dk = {
  texts: (id: string) => ["studio_layer_texts", id] as const,
  pick: (id: string, section: string, q: string, hidden = false) => ["studio_pick", id, section, q, hidden] as const,
  fields: (section: string) => ["studio_fields", section] as const,
};

/** Every layer file with its text (layers are small: a few JSON files). */
export function useLayerTexts(packId: string) {
  return useQuery({
    queryKey: dk.texts(packId),
    queryFn: async () => {
      const files = await api.studioLayerFiles(packId);
      const texts = await Promise.all(files.map((f) => api.studioLayerRead(packId, f.path).catch(() => "")));
      return Object.fromEntries(files.map((f, i) => [f.path, texts[i]])) as Record<string, string>;
    },
    retry: false,
  });
}

export const useLint = (packId: string) => useQuery({ queryKey: sk.lint(packId), queryFn: () => api.studioLayerLint(packId), retry: false });

/** After a write or delete: the lint it answered, the files again, the pack (its file count). */
export function afterWrite(qc: QueryClient, packId: string, report: LintReport) {
  qc.setQueryData(sk.lint(packId), report);
  void qc.invalidateQueries({ queryKey: dk.texts(packId) });
  void qc.invalidateQueries({ queryKey: sk.layer(packId) });
  void qc.invalidateQueries({ queryKey: sk.get(packId) });
  void qc.invalidateQueries({ queryKey: ["studio_pick", packId] });
}

export function useWriteLayer(packId: string) {
  const qc = useQueryClient();
  return {
    write: async (path: string, text: string) => {
      const r = await api.studioLayerWrite(packId, path, text);
      qc.setQueryData<Record<string, string>>(dk.texts(packId), (old) => ({ ...(old ?? {}), [path]: text }));
      afterWrite(qc, packId, r);
      return r;
    },
    remove: async (path: string) => { const r = await api.studioLayerDelete(packId, path); afterWrite(qc, packId, r); return r; },
  };
}

/** The game's recipe input categories (the recipe editor's "Any of" ingredient; studio_categories), read again with the
 *  catalog (useCatalogUpkeep). */
export const useCategories = () => useQuery({ queryKey: ["studio_categories"] as const, queryFn: api.studioCategories, retry: false, staleTime: 5 * 60_000 });

export const useFields = (section: string) => useQuery({ queryKey: dk.fields(section), queryFn: () => api.studioFields(section), retry: false, staleTime: Infinity });

/** "Show hidden game entries" (dev.30.2, netjunk): the pickers leave out the game's old, test and unused entries
 *  (kiln_manage::catalog::is_hidden_name) unless this is on; remembered on this PC. */
const HIDDEN_KEY = "kiln.studio.showHidden";
export const useShowHidden = create<{ on: boolean; set: (on: boolean) => void }>((set) => ({
  on: (() => { try { return localStorage.getItem(HIDDEN_KEY) === "1"; } catch { return false; } })(),
  set: (on) => { try { localStorage.setItem(HIDDEN_KEY, on ? "1" : "0"); } catch { /* private mode: this run only */ } set({ on }); },
}));

/** The picker's hits for a query (debounced); hidden game entries only while "Show hidden game entries" is on. */
export function usePick(packId: string, section: string, query: string, on = true) {
  const [q, setQ] = useState(query);
  const hidden = useShowHidden((s) => s.on);
  useEffect(() => { const h = setTimeout(() => setQ(query), 160); return () => clearTimeout(h); }, [query]);
  return useQuery({ queryKey: dk.pick(packId, section, q.trim(), hidden), queryFn: () => api.studioPick(packId, section, q.trim(), 40, hidden), enabled: on, retry: false, staleTime: 30_000 });
}
/** One entry by its exact name (the icon and display name of a chosen reference; a hidden game entry typed or pasted
 *  is found too). */
export function useEntry(packId: string, section: string, name: string): StudioPickHit | undefined {
  const n = name.trim();
  const plain = !!n && !/[*?]/.test(n);
  const q = useQuery({ queryKey: dk.pick(packId, section, n, true), queryFn: () => api.studioPick(packId, section, n, 40, true), enabled: plain, retry: false, staleTime: 30_000 });
  return q.data?.find((h) => h.name === n || h.id === n);
}

// ---------------------------------------------------------------- drafts (kept while the creator moves between sub-tabs)

export interface Draft<T> { value: T; base: string | undefined; dirty: boolean }
interface DraftStore { by: Record<string, Draft<unknown>>; set: (key: string, d: Draft<unknown> | null) => void }
export const useDrafts = create<DraftStore>((set) => ({
  by: {},
  set: (key, d) => set((s) => { const by = { ...s.by }; if (d) by[key] = d; else delete by[key]; return { by }; }),
}));

/** An editor's value: its unsaved draft, else what the file holds now. */
export function useDraft<T>(key: string, fileText: string | undefined, derive: (text: string | undefined) => T) {
  const d = useDrafts((s) => s.by[key]) as Draft<T> | undefined;
  const set = useDrafts((s) => s.set);
  const value = d && (d.dirty || d.base === fileText) ? d.value : derive(fileText);
  return {
    value,
    dirty: !!d?.dirty,
    change: (v: T) => set(key, { value: v, base: fileText, dirty: true }),
    saved: () => set(key, null),
  };
}
