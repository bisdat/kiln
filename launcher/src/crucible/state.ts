// Crucible's shared state in the launcher: the queries (status, instances, updates, shares), what is open (the pack
// preview, the update screen), the share task that outlives its popover, and the player wording of Crucible's errors
// (DESIGN 1.6). Desktop and Deck both use it.
import { useQuery, useQueryClient, type QueryClient } from "@tanstack/react-query";
import { useEffect } from "react";
import { create } from "zustand";
import * as api from "../api/commands";
import type { CrucibleShareResult, GameId, KilnError, ProfileId } from "../api/commands";
import { invalidateGame, qk } from "../api/hooks";
import { t } from "../i18n";

export const crk = {
  status: ["crucible_status"] as const,
  instances: ["pack_instances"] as const,
  updates: (g: GameId) => ["crucible_updates", g] as const,
  shared: (p: ProfileId) => ["crucible_shared", p] as const,
  open: (target: string) => ["crucible_open", target] as const,
  plan: (p: ProfileId) => ["crucible_update_plan", p] as const,
  trust: ["crucible_trust"] as const,
};

export const useCrucibleStatus = () => useQuery({ queryKey: crk.status, queryFn: api.crucibleStatus, staleTime: 30_000, refetchInterval: 60_000, retry: false });
/** Crucible's surfaces show only when this build has it on. */
export const useCrucibleOn = () => useCrucibleStatus().data?.enabled === true;
export const useInstances = (enabled = true) => {
  const q = useQuery({ queryKey: crk.instances, queryFn: api.packInstances, enabled, retry: false });
  return { ...q, byProfile: new Map(q.data ?? []) };
};
/** Instances with a newer version (the backend caches 5 min and checks on start and every 6 h). */
export const useUpdates = (gameId: GameId | null | undefined, enabled = true) =>
  useQuery({ queryKey: crk.updates(gameId ?? ""), queryFn: () => api.crucibleUpdates(gameId!), enabled: !!gameId && enabled, refetchInterval: 5 * 60_000, retry: false });
/** What the profile's shared pack says now (null: never shared); `stamp` (the profile's updated time) asks again after a change. */
export const useShared = (profileId: ProfileId | null | undefined, stamp?: string, enabled = true) =>
  useQuery({ queryKey: [...crk.shared(profileId ?? ""), stamp ?? ""], queryFn: () => api.crucibleShared(profileId!), enabled: !!profileId && enabled, retry: false });

/** Everything a Crucible change touches. */
export function refreshCrucible(qc: QueryClient, gameId?: GameId | null) {
  for (const k of [crk.instances, ["crucible_updates"], ["crucible_shared"], ["crucible_open"], ["crucible_update_plan"], qk.profiles, ["library_list"], ["profile_resolve"], ["profile_get"]])
    qc.invalidateQueries({ queryKey: k });
  if (gameId) invalidateGame(qc, gameId);
}

/** Follows sign-ins and sign-outs (the community session is Crucible's): the status is asked again. */
export function useStatusFollowsSignIn() {
  const qc = useQueryClient();
  useEffect(() => {
    let off: (() => void) | undefined, live = true;
    api.onKilnEvent("kiln://community", (e) => { if (e.kind === "changed") void qc.invalidateQueries({ queryKey: crk.status }); })
      .then((f) => (live ? (off = f) : f())).catch(() => {});
    return () => { live = false; off?.(); };
  }, [qc]);
}

// ---------------------------------------------------------------- what is open

/** A pack to preview: one on Crucible (a code, a link, a version id) or a dropped .kilnpack file. */
export type PreviewSource = { kind: "crucible"; target: string } | { kind: "file"; path: string };
interface CrucibleUi {
  preview: PreviewSource | null;
  /** The update screen of this instance. */
  update: ProfileId | null;
  openPreview: (s: PreviewSource) => void;
  openUpdate: (profileId: ProfileId) => void;
  closePreview: () => void;
  closeUpdate: () => void;
}
export const useCrucibleUi = create<CrucibleUi>((set) => ({
  preview: null,
  update: null,
  openPreview: (preview) => set({ preview, update: null }),
  openUpdate: (update) => set({ update, preview: null }),
  closePreview: () => set({ preview: null }),
  closeUpdate: () => set({ update: null }),
}));

// ---------------------------------------------------------------- sharing

/** The share that runs now (or ran last), by profile: it outlives the popover (closing it keeps uploading). */
export interface ShareRun {
  profileId: ProfileId; taskId: string | null; result: CrucibleShareResult | null; error: KilnError | null; copied: boolean | null; seen: boolean;
  /** Waiting for the Discord sign-in the share asked for: the share starts once signed in. */
  pendingSignIn?: boolean;
}
export const useShareRun = create<{ runs: Record<string, ShareRun>; set: (r: ShareRun) => void; clear: (pid: string) => void }>((set) => ({
  runs: {},
  set: (r) => set((s) => ({ runs: { ...s.runs, [r.profileId]: r } })),
  clear: (pid) => set((s) => { const runs = { ...s.runs }; delete runs[pid]; return { runs }; }),
}));

/** Copies text; false when the clipboard is out of reach. */
export async function copyText(text: string): Promise<boolean> {
  try {
    if (!navigator.clipboard) return false;
    await navigator.clipboard.writeText(text);
    return true;
  } catch { return false; }
}

/** What the clipboard holds now, if it can be read (a Got a code? prefill; never an error). */
export async function readClipboard(): Promise<string | null> {
  try { return navigator.clipboard?.readText ? await navigator.clipboard.readText() : null; } catch { return null; }
}

/** The share link without its scheme, as the popover shows it. */
export const shortLink = (url: string) => url.replace(/^https?:\/\//, "");

// ---------------------------------------------------------------- player wording

/** Crucible's errors as a player reads them (DESIGN 1.6). */
export function crucibleError(e: unknown): string {
  const k = api.toKilnError(e);
  switch (k.code) {
    case "not_found": return t("crucible.err.notFound");
    case "network": return t("crucible.err.offline");
    case "refused": return k.message || t("crucible.err.gone");
    case "hash_mismatch": return t("crucible.err.badDownload");
    case "game_running": return t("crucible.err.gameRunning");
    case "auth_failed": return t("crucible.share.signInText");
    case "timeout": return t("crucible.err.offline");
    default: return k.message;
  }
}
