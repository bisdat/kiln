// Settings > Game & Kiln, for the selected game: the installs the launcher knows (default game, save backups), Kiln in the
// game (kiln.json's look, size, chat and outlines) and which ShroudForge runs this game's ShroudForge mods.
import { useMutation, useQueryClient } from "@tanstack/react-query";
import { FolderPlus, RefreshCw, Trash2 } from "lucide-react";
import { useState } from "react";
import * as api from "../../../api/commands";
import { qk, useGame, useGameStatus, useSettings } from "../../../api/hooks";
import { StartModal, UnknownBuild, useStartWord } from "../../../components/GameStart";
import { ShroudForgeCard } from "../../../components/ShroudForgeSettings";
import { useT } from "../../../i18n";
import { Badge, Button, ConfirmDanger, EmptyState, IconButton, NumberInput, PathPicker, SchemaForm, Select, Skeleton, errorText, toast } from "../../../ui";
import { useKilnJson } from "../kilnJson";
import { CardGroup, Group, SettingRow, useSettingsView, useShown } from "../parts";
import { kilnGroupOf, kilnLabels, kilnSchema } from "../searchIndex";
import { useSetSettings } from "./Mods";

/** Windows (WebView2): path examples in Windows form. */
export const WIN = /Windows/i.test(navigator.userAgent);

export function GameSection() {
  return (
    <>
      <GamesGroup />
      <KilnGameGroup />
      <CardGroup id="shroudforge"><ShroudForgeCard /></CardGroup>
    </>
  );
}

function GamesGroup() {
  const t = useT();
  const qc = useQueryClient();
  const { games } = useGame();
  const s = useSettings();
  const set = useSetSettings();
  const showList = useShown("games.list");
  const [path, setPath] = useState("");
  const [rm, setRm] = useState<api.GameInstall | null>(null);
  const detect = useMutation({ mutationFn: api.gameDetect, onSuccess: (g) => { qc.setQueryData(qk.games, g); toast.ok(t("settings.games.found", { count: g.length })); }, onError: (e) => toast.error(e) });
  const [startFor, setStartFor] = useState<{ game: api.GameInstall; added: boolean } | null>(null);
  const add = useMutation({ mutationFn: () => api.gameAdd(path.trim()), onSuccess: (g) => {
    setPath(""); qc.invalidateQueries({ queryKey: qk.games });
    // a folder no Steam library knows: how it starts is asked right away (directly by default)
    const custom = g.kind === "client" && g.source === "manual" && !g.library;
    toast.ok(custom ? t("play.addGame.addedCustom") : t("settings.games.added"));
    if (custom) setStartFor({ game: g, added: true });
  }, onError: (e) => toast.error(e) });
  const v = s.data;
  return (
    <Group id="games" items={["games.list", "games.add", "defaultGame", "savesKeep"]} hint={t("settings.rows.games")}
      actions={<Button size="s" icon={<RefreshCw />} loading={detect.isPending} onClick={() => detect.mutate()}>{t("settings.games.detect")}</Button>}>
      {showList && (games.length === 0 ? <span className="k-hint k-st-note">{t("settings.games.none")}</span> : (
        <div className="k-list k-st-items" data-setting="games.list">
          {games.map((g) => <GameRow key={g.id} game={g} onForget={() => setRm(g)} onStart={() => setStartFor({ game: g, added: false })} />)}
        </div>
      ))}
      <SettingRow id="games.add" htmlFor="st-add-game"
        control={(
          <div className="k-input-group k-st-path">
            <div style={{ flex: 1, minWidth: 0 }}><PathPicker id="st-add-game" value={path} onChange={setPath} directory placeholder={WIN ? "D:\\SteamLibrary\\steamapps\\common\\Enshrouded" : "…/common/Enshrouded"} /></div>
            <IconButton label={t("common.add")} variant="secondary" icon={<FolderPlus />} disabled={!path.trim()} loading={add.isPending} onClick={() => add.mutate()} />
          </div>
        )} />
      {v && (
        <SettingRow id="defaultGame" htmlFor="st-game"
          control={<Select id="st-game" value={v.defaultGameId ?? ""} onChange={(id) => set({ defaultGameId: id || null })} style={{ width: 230 }}
            options={[{ value: "", label: t("settings.launcher.firstFound") }, ...games.filter((g) => g.kind === "client").map((g) => ({ value: g.id, label: g.path.split(/[\\/]/).pop() ?? g.id }))]} />} />
      )}
      {v && (
        <SettingRow id="savesKeep" htmlFor="st-keep"
          control={<NumberInput id="st-keep" integer min={1} max={100} value={v.savesKeep} onCommit={(n) => set({ savesKeep: n })} style={{ width: 96 }} />} />
      )}
      {startFor && <StartModal game={startFor.game} added={startFor.added} open onClose={() => setStartFor(null)} />}
      <ConfirmDanger open={!!rm} onClose={() => setRm(null)} title={t("settings.games.forgetAsk")} word={t("common.confirmWord.forget")} action={t("settings.games.forget")} message={t("settings.games.forgetMessage")}
        onConfirm={async () => { try { await api.gameRemove(rm!.id); qc.invalidateQueries({ queryKey: qk.games }); } catch (e) { toast.error(e); throw e; } }} />
    </Group>
  );
}

/** One install in Settings' game list: its folder, kind, where it came from (a custom folder says so), how it starts,
 *  and a build Kiln does not know (dev.29.17). */
function GameRow({ game: g, onForget, onStart }: { game: api.GameInstall; onForget: () => void; onStart: () => void }) {
  const t = useT();
  const status = useGameStatus(g.kind === "client" ? g.id : null);
  const word = useStartWord();
  const custom = g.kind === "client" && g.source === "manual" && !g.library;
  const s = status.data;
  return (
    <div className="k-list-item" data-testid={`game-row-${g.id}`}>
      <span className="k-stack" style={{ gap: 1, flex: 1, minWidth: 0 }}>
        <span className="k-row" style={{ gap: 6 }}>
          <span className="k-item-title">{g.path.split(/[\\/]/).pop()}</span><Badge>{g.kind}</Badge>
          {custom ? <Badge tone="ember">{t("play.source.custom")}</Badge> : <Badge>{g.source}</Badge>}
          {s?.start && <button type="button" className="k-link" onClick={onStart} title={t("settings.games.startTip")}>{t("play.startsVia", { how: word(s.start) })}</button>}
        </span>
        <span className="k-item-sub k-mono k-ellipsis" title={g.path}>{g.path}</span>
        <UnknownBuild build={s?.build} compact />
      </span>
      <IconButton label={t("settings.games.forget")} size="s" icon={<Trash2 />} onClick={onForget} />
    </div>
  );
}

/** kiln.json's in-game entries (look, theme, background, size, chat history and completion, outlines); the rest of the
 *  file is in Advanced > Kiln's file, graphics in Graphics, ShroudForge below, the language in General. */
function KilnGameGroup() {
  const t = useT();
  const { game } = useGame();
  const status = useGameStatus(game?.id);
  const k = useKilnJson(game?.id);
  const v = useSettingsView();
  if (!game) return null;
  const schema = k.doc.data?.schema;
  const ids = (schema?.entries ?? []).filter((e) => kilnGroupOf(e.path) === "kiln").map((e) => `kj:${e.path}`);
  const only = v.hits ? new Set([...v.hits].filter((h) => h.startsWith("kj:"))) : null;
  const installed = status.data?.runtime.installed;
  return (
    <Group id="kiln" items={ids} hint={t("settings.groups.kilnHint")}>
      {status.data && !installed ? <EmptyState title={t("settings.kilnJson.notInstalled")} hint={t("settings.kilnJson.notInstalledHint")} /> :
        k.doc.isLoading ? <Skeleton lines={4} /> : k.doc.error ? <EmptyState title={t("settings.kilnJson.unavailable")} hint={errorText(k.doc.error)} /> : schema && (
          <SchemaForm clamp schema={kilnSchema(schema, "kiln", only)} values={k.doc.data!.values} errors={k.doc.data!.errors} disabled={k.busy} labels={kilnLabels()}
            onChange={(path, value) => k.set.mutate({ path, value })} onReset={(path) => k.reset.mutate(path)} />
        )}
    </Group>
  );
}
