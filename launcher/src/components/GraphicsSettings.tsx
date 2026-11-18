// Settings, Graphics (dev.29.18): Kiln's own graphics tools in the game, the "graphics" section of the game's
// kiln/kiln.json (frame cap, DLSS preset and render scale, readout). The same document, query and mutations as the
// kiln.json card (pages/SettingsPage.tsx, which leaves these entries out); the game reads the file again while it runs.
// Then (phase B) "Behind the game's FSR 3": the upscaler Kiln runs in place of the game's FSR 3 (graphics.upscaler,
// graphics.amdUpscaler), what runs now or ran in the last game (GameStatus.graphicsState, the game's
// kiln/state/graphics.json) or what will run, and the offer of the chosen vendor's files when they are not in the
// game; under it the upscaler files (components/UpscalerSettings.tsx: licence first, progress, Remove).
// At the end: the tools in the game folder next to which Kiln's frame cap, DLSS settings and upscaler step aside
// (GameStatus.graphicsStepAside, kiln_manage::game::graphics_step_aside).
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { ConfigDocument, GameId, GraphicsNow, GraphicsStepAside, Json, SchemaSnapshot } from "../api/commands";
import { qk, useGame, useGameStatus } from "../api/hooks";
import { t as tr, useT } from "../i18n";
import { Banner, Button, Card, EmptyState, Field, ProgressBar, SchemaForm, Select, Skeleton, errorText, toast } from "../ui";
import { AMD_MODES, UPSCALER_CHOICES, amdModeOf, choiceOf, upscalerView } from "./upscalerStatus";
import { UpscalerFilesList, useUpscalerFiles, type UpscalerFiles } from "./UpscalerSettings";

/** The kiln.json section the Graphics card edits. */
export const GRAPHICS_SECTION = "graphics";

/** A kiln.json entry of the Graphics card (`graphics.*`). */
export const isGraphicsPath = (path: string) => path.startsWith(GRAPHICS_SECTION + ".");

/** (phase B) The kiln.json entries of the upscaler choice: the card's own block, not the form. */
export const UPSCALER_PATHS = ["graphics.upscaler", "graphics.amdUpscaler"];

/** The Graphics card's form: the `graphics.*` entries alone but the upscaler's, no section head (the card's title says it). */
export function graphicsSchema(schema: SchemaSnapshot): SchemaSnapshot {
  return { ...schema, sections: [], entries: schema.entries.filter((e) => isGraphicsPath(e.path) && !UPSCALER_PATHS.includes(e.path)) };
}

/** The card's field labels by kiln.json path (SchemaForm's `labels`): the key names ("fpsCap", "dlssScale") do not read as
 *  words. Every `graphics.*` entry of the snapshot has one (unit/graphics-settings.test.tsx). */
export function graphicsLabels(): Record<string, string> {
  return {
    "graphics.fpsCap": tr("settings.graphics.labels.fpsCap"),
    "graphics.dlssPreset": tr("settings.graphics.labels.dlssPreset"),
    "graphics.dlssScale": tr("settings.graphics.labels.dlssScale"),
    "graphics.dlssRenderScale": tr("settings.graphics.labels.dlssRenderScale"),
    "graphics.dlssQualityScale": tr("settings.graphics.labels.dlssQualityScale"),
    "graphics.dlssBalancedScale": tr("settings.graphics.labels.dlssBalancedScale"),
    "graphics.dlssPerformanceScale": tr("settings.graphics.labels.dlssPerformanceScale"),
    "graphics.dlssUltraPerformanceScale": tr("settings.graphics.labels.dlssUltraPerformanceScale"),
    "graphics.readout": tr("settings.graphics.labels.readout"),
  };
}

/** One line for a tool Kiln's graphics tools step aside for: what it is (naming the file) and what steps aside. */
export function graphicsAsideText(a: GraphicsStepAside): string {
  const tools: Record<string, string> = {
    optiscaler: tr("settings.graphics.aside.optiscaler", { file: a.file }),
    streamline: tr("settings.graphics.aside.streamline", { file: a.file }),
    vkfg: tr("settings.graphics.aside.vkfg", { file: a.file }),
    dlsstweaks: tr("settings.graphics.aside.dlsstweaks", { file: a.file }),
    nvngx: tr("settings.graphics.aside.nvngx", { file: a.file }),
    upscaler: tr("settings.graphics.aside.upscaler", { file: a.file }),
  };
  const tool = tools[a.kind] ?? tr("settings.graphics.aside.unknown", { file: a.file });
  const effect = tr(`settings.graphics.aside.${a.frameCap ? (a.upscaler ? "frameCapUpscaler" : "frameCap") : a.upscaler ? "upscalerOnly" : "dlss"}`);
  return tr("settings.graphics.aside.line", { tool, effect });
}

export function GraphicsCard() {
  const { game } = useGame();
  // a dedicated server draws no frames: the card is for a client
  if (!game || game.kind !== "client") return null;
  return <Graphics gameId={game.id} />;
}

function Graphics({ gameId }: { gameId: GameId }) {
  const t = useT();
  const qc = useQueryClient();
  const status = useGameStatus(gameId);
  const key = qk.kilnJson(gameId);
  const doc = useQuery({ queryKey: key, queryFn: () => api.configKilnRead(gameId) });
  const onDoc = (d: ConfigDocument) => qc.setQueryData(key, d);
  const set = useMutation({ mutationFn: (v: { path: string; value: Json }) => api.configKilnSet(gameId, v.path, v.value), onSuccess: onDoc, onError: (e) => toast.error(e) });
  const reset = useMutation({ mutationFn: (path: string) => api.configKilnReset(gameId, path), onSuccess: onDoc, onError: (e) => toast.error(e) });
  const files = useUpscalerFiles(gameId);
  const installed = status.data?.runtime.installed;
  const aside = status.data?.graphicsStepAside ?? [];
  const schema = doc.data?.schema ? graphicsSchema(doc.data.schema) : null;
  const busy = set.isPending || reset.isPending;
  return (
    <Card id="graphics" title={t("settings.graphics.title")} sub={t("settings.graphics.sub")} aria-label={t("settings.graphics.title")}>
      {status.data && !installed ? <EmptyState title={t("settings.kilnJson.notInstalled")} hint={t("settings.kilnJson.notInstalledHint")} /> :
        doc.isLoading ? <Skeleton lines={4} /> : doc.error ? <EmptyState title={t("settings.kilnJson.unavailable")} hint={errorText(doc.error)} /> : schema && (
          <>
            <SchemaForm schema={schema} values={doc.data!.values} errors={doc.data!.errors.filter((e) => isGraphicsPath(e.path) && !UPSCALER_PATHS.includes(e.path))} disabled={busy} labels={graphicsLabels()}
              onChange={(path, value) => set.mutate({ path, value })} onReset={(path) => reset.mutate(path)} />
            <UpscalerChoiceBlock values={doc.data!.values} disabled={busy} onSet={(path, value) => set.mutate({ path, value })}
              state={status.data?.graphicsState} aside={aside} files={files} running={!!status.data?.running} />
            <UpscalerFilesList files={files} />
          </>
        )}
      {files.dialogs}
      {aside.length > 0 && (
        <div className="k-stack" style={{ gap: 6 }}>
          {aside.map((a) => (
            <Banner key={`${a.kind}-${a.file}`} tone="info">
              <span data-testid="graphics-step-aside">{graphicsAsideText(a)}</span>
            </Banner>
          ))}
        </div>
      )}
    </Card>
  );
}

/** "Behind the game's FSR 3": the choice, AMD's version, the offer of the chosen vendor's files, what runs and why. */
export function UpscalerChoiceBlock({ values, disabled, onSet, state, aside, files, running }: {
  values: Json; disabled?: boolean; onSet: (path: string, value: Json) => void; state?: GraphicsNow; aside: GraphicsStepAside[]; files: UpscalerFiles;
  /** The game runs: the launcher downloads nothing into it now (vendor_download refuses). */
  running?: boolean;
}) {
  const t = useT();
  const choice = choiceOf(values);
  const amd = amdModeOf(values);
  const view = upscalerView(choice, amd, state, files.status.data?.components, aside);
  const offer = view.offer;
  const task = offer ? files.task(offer) : undefined;
  const downloading = !!task && (task.state === "running" || task.state === "queued");
  return (
    <div className="k-stack k-upscaler-choice" style={{ gap: 8 }} role="group" aria-label={t("settings.graphics.upscaler.title")} data-testid="upscaler-choice">
      <span className="k-panel-title">{t("settings.graphics.upscaler.title")}</span>
      <Field label={t("settings.graphics.upscaler.label")} hint={t(`settings.graphics.upscaler.choiceHint.${choice}`)} htmlFor="gfx-upscaler">
        <Select style={{ maxWidth: 420 }} id="gfx-upscaler" value={choice} disabled={disabled} onChange={(v) => onSet("graphics.upscaler", v)}
          options={UPSCALER_CHOICES.map((v) => ({ value: v, label: t(`settings.graphics.upscaler.choice.${v}`) }))} />
      </Field>
      {choice === "amd" && (
        <Field label={t("settings.graphics.upscaler.amdLabel")} hint={t("settings.graphics.upscaler.amdHint")} htmlFor="gfx-amd-upscaler">
          <Select style={{ maxWidth: 420 }} id="gfx-amd-upscaler" value={amd} disabled={disabled} onChange={(v) => onSet("graphics.amdUpscaler", v)}
            options={AMD_MODES.map((v) => ({ value: v, label: t(`settings.graphics.upscaler.amd.${v}`) }))} />
        </Field>
      )}
      {offer && (
        <Banner tone="warn" actions={downloading
          ? <Button size="s" variant="ghost" aria-label={t("settings.upscalers.actionLabel.cancel", { name: offer.name })} onClick={() => files.cancel(offer)}>{t("common.cancel")}</Button>
          : <Button size="s" variant="primary" disabled={running} onClick={() => files.download(offer)}>{t("settings.upscalers.actionLabel.download", { name: offer.name })}</Button>}>
          <span data-testid="upscaler-offer">{t(offer.state === "damaged" ? "settings.graphics.upscaler.offerDamaged" : "settings.graphics.upscaler.offer", { name: offer.name })}</span>
          {running && !downloading && <span className="k-hint" data-testid="upscaler-offer-running"> {t("settings.graphics.upscaler.offerRunning")}</span>}
          {downloading && <ProgressBar value={task!.progress ?? 0} />}
        </Banner>
      )}
      {view.heading && (
        <div className="k-stack" style={{ gap: 2 }} data-testid="upscaler-status" data-heading={view.heading}>
          <span className="k-label">{t(`settings.graphics.upscaler.heading.${view.heading}`)}</span>
          {view.lines.map((l) => <span key={l} className="k-upscaler-detail" data-testid="upscaler-status-line">{l}</span>)}
          {view.detail && <span className="k-hint" data-testid="upscaler-status-detail">{t("settings.graphics.upscaler.detail", { detail: view.detail })}</span>}
        </div>
      )}
      <span className="k-hint">{t("settings.graphics.upscaler.hint")}</span>
    </div>
  );
}
