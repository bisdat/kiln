// The selected game's kiln/kiln.json for Settings' groups that edit it (Game & Kiln > Kiln in the game, Graphics, Advanced >
// Kiln's file): one query (the same key as the language row and the ShroudForge card), config_kiln_set and _reset.
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../../api/commands";
import type { ConfigDocument, GameId, Json } from "../../api/commands";
import { qk } from "../../api/hooks";
import { toast } from "../../ui";

export function useKilnJson(gameId: GameId | null | undefined) {
  const qc = useQueryClient();
  const key = qk.kilnJson(gameId ?? "");
  const doc = useQuery({ queryKey: key, queryFn: () => api.configKilnRead(gameId!), enabled: !!gameId });
  const onDoc = (d: ConfigDocument) => qc.setQueryData(key, d);
  const set = useMutation({ mutationFn: (v: { path: string; value: Json }) => api.configKilnSet(gameId!, v.path, v.value), onSuccess: onDoc, onError: (e) => toast.error(e) });
  const reset = useMutation({ mutationFn: (path: string) => api.configKilnReset(gameId!, path), onSuccess: onDoc, onError: (e) => toast.error(e) });
  return { doc, set, reset, busy: set.isPending || reset.isPending };
}
