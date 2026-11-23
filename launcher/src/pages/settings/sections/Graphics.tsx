// Settings > Graphics, for the selected game (a client): the tools Kiln's graphics tools step aside for, at the top;
// Kiln's graphics tools (kiln.json's "graphics" section: frame cap, DLSS preset and render size, readout); "Behind the
// game's FSR 3": the upscaler Kiln runs in place of the game's FSR 3 (graphics.upscaler, graphics.amdUpscaler), what runs
// now or ran in the last game (GameStatus.graphicsState) or what will run, and the offer of the chosen vendor's files;
// the upscaler files (components/UpscalerSettings.tsx: licence first, progress, Remove).
import type { GameId, GraphicsNow, GraphicsStepAside, Json } from "../../../api/commands";
import { useGame, useGameStatus } from "../../../api/hooks";
import { graphicsAsideText, graphicsLabels, graphicsSchema, isGraphicsPath, UPSCALER_PATHS } from "../../../components/GraphicsSettings";
import { UpscalerFilesList, useUpscalerFiles, type UpscalerFiles } from "../../../components/UpscalerSettings";
import { AMD_MODES, UPSCALER_CHOICES, amdModeOf, choiceOf, upscalerView } from "../../../components/upscalerStatus";
import { useT } from "../../../i18n";
import { Banner, Button, EmptyState, ProgressBar, SchemaForm, Select, Skeleton, errorText } from "../../../ui";
import { useKilnJson } from "../kilnJson";
import { moreBeyond, textOr } from "../model";
import { Group, SettingRow, useSettingsView } from "../parts";

export function GraphicsSection() {
  const t = useT();
  const v = useSettingsView();
  const { game, games } = useGame();
  // a dedicated server draws no frames: the section is for a client (the selected game is always one when the launcher
  // knows one: useGame), so with none, it says what is missing and leads to the list of installs
  if (!game || game.kind !== "client") {
    const server = games.some((g) => g.kind === "server");
    return (
      <EmptyState title={t(server ? "settings.noClientServer" : "settings.noClient")} hint={t(server ? "settings.noClientServerHint" : "settings.noClientHint")}
        action={<Button size="s" onClick={() => v.jump("game", "games")}>{t("settings.openGames")}</Button>} />
    );
  }
  return <Graphics key={game.id} gameId={game.id} />;
}

function Graphics({ gameId }: { gameId: GameId }) {
  const t = useT();
  const v = useSettingsView();
  const status = useGameStatus(gameId);
  const k = useKilnJson(gameId);
  const files = useUpscalerFiles(gameId);
  const installed = status.data?.runtime.installed;
  const aside = status.data?.graphicsStepAside ?? [];
  const doc = k.doc.data;
  const schema = doc?.schema ? graphicsSchema(doc.schema) : null;
  const ids = (schema?.entries ?? []).map((e) => `kj:${e.path}`);
  const shown = v.hits ? schema && { ...schema, entries: schema.entries.filter((e) => v.hits!.has(`kj:${e.path}`)) } : schema;
  const blocked = status.data && !installed ? <EmptyState title={t("settings.kilnJson.notInstalled")} hint={t("settings.kilnJson.notInstalledHint")} />
    : k.doc.isLoading ? <Skeleton lines={4} /> : k.doc.error ? <EmptyState title={t("settings.kilnJson.unavailable")} hint={errorText(k.doc.error)} /> : null;
  const running = !!status.data?.running;
  return (
    <>
      {!v.hits && aside.length > 0 && (
        <div className="k-stack k-st-notes" style={{ gap: 6 }}>
          {aside.map((a) => (
            <Banner key={`${a.kind}-${a.file}`} tone="info">
              <span data-testid="graphics-step-aside">{graphicsAsideText(a)}</span>
            </Banner>
          ))}
        </div>
      )}
      <Group id="graphics" items={ids} hint={t("settings.groups.graphicsHint")} more={moreBeyond(t("settings.groups.graphicsHint"), textOr("settings.groups.graphicsMore", "settings.graphics.sub"))}>
        {blocked ?? (shown && (
          <SchemaForm clamp schema={shown} values={doc!.values} errors={doc!.errors.filter((e) => isGraphicsPath(e.path) && !UPSCALER_PATHS.includes(e.path))} disabled={k.busy}
            labels={graphicsLabels()} onChange={(path, value) => k.set.mutate({ path, value })} onReset={(path) => k.reset.mutate(path)} />
        ))}
      </Group>
      {!blocked && doc && (
        <UpscalerGroup values={doc.values} disabled={k.busy} onSet={(path, value) => k.set.mutate({ path, value })}
          state={status.data?.graphicsState} aside={aside} files={files} running={running} />
      )}
      {!blocked && (
        <Group id="upscalers" items={["upscalers"]} hint={t("settings.rows.upscalers")}
          more={<><p className="k-st-p">{t("settings.upscalers.sub")}</p><p className="k-st-p" data-testid="upscalers-purpose">{t("settings.upscalers.purpose")}</p></>}>
          <UpscalerFilesList files={files} running={running} bare />
        </Group>
      )}
      {files.dialogs}
    </>
  );
}

/** "Behind the game's FSR 3": the choice, AMD's version, the offer of the chosen vendor's files, what runs and why. */
function UpscalerGroup({ values, disabled, onSet, state, aside, files, running }: {
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
    <Group id="upscaler" items={["upscaler.choice", "upscaler.amd"]} hint={t("settings.rows.upscalerGroup")} more={moreBeyond(t("settings.rows.upscalerGroup"), t("settings.graphics.upscaler.hint"))} testId="upscaler-choice">
      <SettingRow id="upscaler.choice" htmlFor="gfx-upscaler"
        control={<Select style={{ width: 240 }} id="gfx-upscaler" value={choice} disabled={disabled} onChange={(x) => onSet("graphics.upscaler", x)}
          options={UPSCALER_CHOICES.map((x) => ({ value: x, label: t(`settings.graphics.upscaler.choice.${x}`) }))} />} />
      {view.restart && (
        <Banner tone="warn">
          <span data-testid="upscaler-restart">{view.restart}</span>
        </Banner>
      )}
      {choice === "amd" && (
        <SettingRow id="upscaler.amd" htmlFor="gfx-amd-upscaler"
          control={<Select style={{ width: 240 }} id="gfx-amd-upscaler" value={amd} disabled={disabled} onChange={(x) => onSet("graphics.amdUpscaler", x)}
            options={AMD_MODES.map((x) => ({ value: x, label: t(`settings.graphics.upscaler.amd.${x}`) }))} />} />
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
        <div className="k-stack k-st-status" style={{ gap: 2 }} data-testid="upscaler-status" data-heading={view.heading}>
          <span className="k-label">{t(`settings.graphics.upscaler.heading.${view.heading}`)}</span>
          {view.lines.map((l) => <span key={l} className="k-upscaler-detail" data-testid="upscaler-status-line">{l}</span>)}
          {view.detail && <span className="k-hint" data-testid="upscaler-status-detail">{t("settings.graphics.upscaler.detail", { detail: view.detail })}</span>}
        </div>
      )}
    </Group>
  );
}
