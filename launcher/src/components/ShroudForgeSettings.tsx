// Settings, ShroudForge: which ShroudForge runs this game's ShroudForge mods, kiln.json
// `shroudforge.backend` (game level, as the core reads it at boot: kiln_manage::sfi_backend, commands/sfi_backend.rs).
// On the Graphics card's pattern: the kiln.json card leaves `shroudforge.*` to this one (pages/SettingsPage.tsx). It
// says what is set (the default, a choice, the older "Use my installed ShroudForge" switch, a value no reader takes),
// what the next start gets and why, and the last start as the core froze it. Kiln's interpreter is greyed while held,
// with the reason; ShroudForge 1.8 is never offered. A choice opens a dialog with its preview (nothing written); Cancel
// writes nothing; Save writes one key over the bytes the card read (bytes changed under an open choice: the card reads
// again and the choice is previewed again on them). The card is keyed by game: an answer for a game it
// no longer shows is dropped. No backend picker anywhere else (Play, the mods list).
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import * as api from "../api/commands";
import type { GameId, SfBackend, SfBackendCard, SfResolution } from "../api/commands";
import { isKilnError } from "../api/commands";
import { qk, useGame, useGameStatus } from "../api/hooks";
import { useT, type TFunction } from "../i18n";
import { Badge, Banner, Button, Card, EmptyState, Modal, Skeleton, errorText, toast } from "../ui";
import { canSave, choiceRows, draftFor, type Draft } from "./shroudForgeBackend";

/** The kiln.json section this card edits; the kiln.json card leaves it out. */
export const isShroudForgePath = (path: string) => path === "shroudforge" || path.startsWith("shroudforge.");

const cardKey = (gameId: GameId) => ["sfi_backend_read", gameId] as const;
const L = "sfBackend.backend";

const nameOf = (t: TFunction, b: SfBackend | string | null | undefined) =>
  b === "hosted" || b === "installed" || b === "kiln" ? t(`${L}.choice.${b}`) : t(`${L}.none`);

export function ShroudForgeCard() {
  const { game } = useGame();
  if (!game || game.kind !== "client") return null;
  // keyed: a game switch starts the card over, and what was asked for the game before goes with it
  return <ShroudForge key={game.id} gameId={game.id} />;
}

function ShroudForge({ gameId }: { gameId: GameId }) {
  const t = useT();
  const qc = useQueryClient();
  const shown = useGame().game?.id ?? null;
  const status = useGameStatus(gameId);
  const card = useQuery({ queryKey: cardKey(gameId), queryFn: () => api.sfiBackendRead(gameId) });
  const [draft, setDraft] = useState<Draft | null>(null);
  const preview = useQuery({
    queryKey: ["sfi_backend_preview", draft?.gameId, draft?.backend, draft?.revision],
    queryFn: () => api.sfiBackendPreview(draft!.gameId, draft!.backend),
    enabled: !!draft, staleTime: 0, gcTime: 0, retry: false,
  });
  const save = useMutation({
    mutationFn: (d: Draft) => api.sfiBackendSet(d.gameId, d.backend, d.revision),
    onSuccess: (c) => {
      qc.setQueryData(cardKey(c.gameId), c);
      void qc.invalidateQueries({ queryKey: qk.kilnJson(c.gameId) });
      toast.ok(t(`${L}.saved`, { name: nameOf(t, c.next.selected) }));
    },
    onError: (e, d) => {
      toast.error(e);
      if (isKilnError(e) && e.code === "conflict") void qc.invalidateQueries({ queryKey: cardKey(d.gameId) });
    },
    onSettled: () => setDraft(null),
  });
  const running = !!status.data?.running;
  const c = card.data;
  // kiln.json changed after the card read it (the kiln.json card on this page, a hand edit): the preview answers for
  // other bytes, so Save would stay greyed. The card reads the file again, and the open choice follows the bytes it
  // reads now (its own preview first, then Save); a choice the change made moot closes.
  const stale = !!draft && !!preview.data && preview.data.gameId === draft.gameId && preview.data.revision !== draft.revision;
  useEffect(() => { if (stale) void qc.invalidateQueries({ queryKey: cardKey(gameId) }); }, [stale, qc, gameId]);
  useEffect(() => {
    if (draft && c && !save.isPending && c.gameId === draft.gameId && c.revision !== draft.revision) setDraft(draftFor(c, draft.backend, running));
  }, [c, draft, running, save.isPending]);
  const ask = (backend: SfBackend | null) => { if (c) setDraft(draftFor(c, backend, running)); };
  const cancel = () => { if (!save.isPending) setDraft(null); };
  return (
    <Card id="shroudforge" title={t(`${L}.title`)} sub={t(`${L}.sub`)} aria-label={t(`${L}.title`)}
      actions={c && c.setting.kind !== "default" && !c.unreadable
        ? <Button size="s" variant="ghost" disabled={running} onClick={() => ask(null)}>{t(`${L}.reset`)}</Button> : undefined}>
      {status.data && !status.data.runtime.installed ? <EmptyState title={t("settings.kilnJson.notInstalled")} hint={t("settings.kilnJson.notInstalledHint")} /> :
        card.isLoading ? <Skeleton lines={4} /> : card.error ? <EmptyState title={t("settings.kilnJson.unavailable")} hint={errorText(card.error)} /> : c && (
          <div className="k-stack" style={{ gap: 10 }}>
            <Now card={c} />
            <div className="k-list" role="radiogroup" aria-label={t(`${L}.label`)} data-testid="sf-backend-choices" style={{ margin: "0 -8px" }}>
              {choiceRows(c, running).map((r) => (
                <button key={r.backend} type="button" role="radio" aria-checked={r.checked} aria-disabled={r.disabled}
                  className="k-list-item" data-testid={`sf-backend-${r.backend}`}
                  style={{ padding: 8, textAlign: "start", width: "100%", background: "none", border: "none", cursor: r.disabled ? "default" : "pointer", opacity: r.held ? 0.55 : 1 }}
                  onClick={() => { if (!r.disabled) ask(r.backend); }}>
                  <span className="k-stack" style={{ gap: 1, flex: 1, minWidth: 0 }}>
                    <span className="k-row" style={{ gap: 6 }}>
                      <span aria-hidden>{r.checked ? "◆" : "◇"}</span>
                      <span className="k-item-title">{t(`${L}.choice.${r.backend}`)}</span>
                      {r.isDefault && <Badge>{t(`${L}.default`)}</Badge>}
                      {r.held && <Badge tone="idle">{t(`${L}.held`)}</Badge>}
                    </span>
                    <span className="k-item-sub">{t(`${L}.about.${r.backend}`)}</span>
                    {r.held && <span className="k-hint" data-testid="sf-backend-held">{t(`${L}.heldWhy`, { reason: r.held })}</span>}
                  </span>
                </button>
              ))}
            </div>
            {running && <span className="k-hint" data-testid="sf-backend-running">{t(`${L}.running`)}</span>}
          </div>
        )}
      <Modal open={!!draft && !!c && draft.gameId === shown} onClose={cancel} title={t(`${L}.ask.title`)} locked={save.isPending}
        footer={<>
          <Button variant="ghost" disabled={save.isPending} onClick={cancel}>{t("common.cancel")}</Button>
          <Button variant="primary" loading={save.isPending} disabled={!canSave(draft, shown, c, preview.data)}
            onClick={() => { if (canSave(draft, shown, c, preview.data)) save.mutate(draft); }}>{t("common.save")}</Button>
        </>}>
        <div className="k-stack" style={{ gap: 8 }} data-testid="sf-backend-ask">
          {c?.setting.kind === "inherited" && <span>{t(`${L}.ask.replaces`)}</span>}
          {preview.isLoading ? <Skeleton lines={2} /> : preview.error ? <Banner tone="err">{errorText(preview.error)}</Banner> : preview.data && (
            preview.data.refused ? <Banner tone="err"><span data-testid="sf-backend-refused">{t(`${L}.ask.refused`, { reason: preview.data.refused })}</span></Banner>
              : preview.data.unchanged ? <span>{t(`${L}.ask.unchanged`)}</span>
              : preview.data.next && <NextLine next={preview.data.next} label="ask.to" />
          )}
          {draft?.backend === "kiln" && <Banner tone="warn"><span data-testid="sf-backend-kiln-warning">{t(`${L}.ask.kiln`)}</span></Banner>}
          <span className="k-hint">{t(`${L}.ask.when`)}</span>
        </div>
      </Modal>
    </Card>
  );
}

/** What is set, what the next start gets, what stands in its way, and the last start. */
function Now({ card: c }: { card: SfBackendCard }) {
  const t = useT();
  const s = c.setting;
  return (
    <div className="k-stack" style={{ gap: 6 }} data-testid="sf-backend-now" data-sf-kind={s.kind}>
      {c.unreadable && <Banner tone="err"><span data-testid="sf-backend-unreadable">{t(`${L}.unreadable`, { reason: c.unreadable })}</span></Banner>}
      {s.kind === "invalid" ? <Banner tone="err"><span data-testid="sf-backend-setting">{t(`${L}.set.invalid`, { value: s.value })}</span></Banner>
        : <span data-testid="sf-backend-setting">{s.kind === "default" ? t(`${L}.set.default`) : t(`${L}.set.${s.kind}`, { name: nameOf(t, s.backend) })}</span>}
      <NextLine next={c.next} label="next" />
      {c.next.source === "envRuntime" && <Banner tone="info"><span data-testid="sf-backend-env">{t(`${L}.envRuntime`)}</span></Banner>}
      {(c.next.envOffNote || c.interpreterRoute === "env-off") && <Banner tone="warn"><span data-testid="sf-backend-env">{t(`${L}.envOff`)}</span></Banner>}
      {c.interpreterRoute === "missing" && <Banner tone="warn"><span data-testid="sf-backend-route">{t(`${L}.route.missing`)}</span></Banner>}
      {c.interpreterRoute === "own-loader" && <Banner tone="warn"><span data-testid="sf-backend-route">{t(`${L}.route.ownLoader`, { file: c.ownLoader ?? "" })}</span></Banner>}
      {c.last && (
        <span className="k-stack" style={{ gap: 1 }} data-testid="sf-backend-last">
          <span>{t(`${L}.last`, { name: nameOf(t, c.last.backend) })}</span>
          {c.last.reason && <span className="k-hint">{c.last.reason}</span>}
        </span>
      )}
    </div>
  );
}

/** "Next start: X" and the core's reason under it. */
function NextLine({ next, label }: { next: SfResolution; label: "next" | "ask.to" }) {
  const t = useT();
  return (
    <span className="k-stack" style={{ gap: 1 }} data-testid={label === "next" ? "sf-backend-next" : "sf-backend-ask-next"} data-selected={next.selected ?? "none"}>
      <span className="k-label">{t(`${L}.${label}`, { name: nameOf(t, next.selected) })}</span>
      <span className="k-hint">{next.reason}</span>
    </span>
  );
}
