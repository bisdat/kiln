// Settings > Advanced: rarely needed, some of it risky. How the launcher draws (Linux), protection (WebKit's sandbox,
// http modpacks), extra Steam folders, the rest of the game's kiln.json, the game art the launcher makes, the developer
// switch (the desktop app only).
import { useQueryClient } from "@tanstack/react-query";
import { Plus, X } from "lucide-react";
import { useState, type ReactNode } from "react";
import * as api from "../../../api/commands";
import { qk, useAppInfo, useGame, useGameStatus, useSettings } from "../../../api/hooks";
import { fallbackEnabled } from "../../../api/fallback";
import { useAssetsStatus, useExtractTask } from "../../../components/GameArt";
import { rich, useT } from "../../../i18n";
import { Badge, EmptyState, IconButton, PathPicker, SchemaForm, Skeleton, Toggle, errorText, fmtAgo, fmtBytes, toast } from "../../../ui";
import { useKilnJson } from "../kilnJson";
import { Group, SettingRow, useSettingsView, useShown } from "../parts";
import { kilnGroupOf, kilnLabels, kilnSchema } from "../searchIndex";
import { WIN } from "./GameKiln";
import { useSetSettings } from "./Mods";

export function AdvancedSection() {
  return (
    <>
      <DrawingGroup />
      <ProtectionGroup />
      <SteamGroup />
      <KilnFileGroup />
      <ArtGroup />
      <DeveloperGroup />
    </>
  );
}

/** On Linux: "Draw the launcher without the GPU" (the render marker, render.rs), on Wayland "Switch to X11 when drawing
 *  is slow on Wayland" (render.rs' X11 switch), and "Force GPU drawing on NVIDIA" (past Ubuntu WebKit's NVIDIA block). */
function DrawingGroup() {
  const t = useT();
  const qc = useQueryClient();
  const info = useAppInfo();
  const render = info.data?.render;
  if (!render) return null;
  const setRender = (on: boolean) => api.appRenderSet(on).then((next) => {
    qc.setQueryData(qk.appInfo, (cur: api.AppInfo | undefined) => (cur ? { ...cur, render: next } : cur));
    toast.ok(on ? t("settings.appearance.nextNoGpu") : t("settings.appearance.nextGpu"));
  }).catch((e) => toast.error(e));
  const setX11 = (on: boolean) => api.appRenderX11Set(on).then((next) => {
    qc.setQueryData(qk.appInfo, (cur: api.AppInfo | undefined) => (cur ? { ...cur, render: next } : cur));
    toast.ok(on ? t("settings.render.x11NextOn") : t("settings.render.x11NextOff"));
  }).catch((e) => toast.error(e));
  const setForce = (on: boolean) => api.appRenderForceSet(on).then((next) => {
    qc.setQueryData(qk.appInfo, (cur: api.AppInfo | undefined) => (cur ? { ...cur, render: next } : cur));
    toast.ok(on ? t("settings.render.forceNextOn") : t("settings.render.forceNextOff"));
  }).catch((e) => toast.error(e));
  const next = t("settings.tag.nextStart");
  return (
    <Group id="drawing" items={["noGpu", "x11", "forceDmabuf"]} hint={t("settings.groups.drawingHint")}>
      <SettingRow id="noGpu" htmlFor="st-no-gpu" tag={next}
        control={<Toggle id="st-no-gpu" checked={render.marker} onChange={(on) => void setRender(on)} />} />
      {render.x11 && (
        <SettingRow id="x11" htmlFor="st-x11" tag={next}
          control={<Toggle id="st-x11" checked={render.x11.auto} disabled={render.x11.why === "yours"} onChange={(on) => void setX11(on)} />} />
      )}
      {render.force && (
        <SettingRow id="forceDmabuf" htmlFor="st-force-dmabuf" tag={next}
          control={<Toggle id="st-force-dmabuf" checked={render.force.on} disabled={render.force.why === "yours"} onChange={(on) => void setForce(on)} />} />
      )}
    </Group>
  );
}

/** WebKit's sandbox (the sandbox marker, sandbox.rs) and modpacks over plain http. */
function ProtectionGroup() {
  const t = useT();
  const qc = useQueryClient();
  const info = useAppInfo();
  const s = useSettings();
  const set = useSetSettings();
  const sandbox = info.data?.sandbox;
  const setSandbox = (on: boolean) => api.appSandboxSet(on).then((next) => {
    qc.setQueryData(qk.appInfo, (cur: api.AppInfo | undefined) => (cur ? { ...cur, sandbox: next } : cur));
    toast.ok(on ? t("settings.sandbox.nextOn") : t("settings.sandbox.nextOff"));
  }).catch((e) => toast.error(e));
  return (
    <Group id="protection" items={["sandbox", "httpModpacks"]}>
      {sandbox && (
        <SettingRow id="sandbox" htmlFor="st-sandbox" tag={t("settings.tag.nextStart")}
          control={<Toggle id="st-sandbox" checked={!sandbox.marker} onChange={(on) => void setSandbox(on)} />} />
      )}
      <SettingRow id="httpModpacks" htmlFor="st-http-modpacks"
        control={<Toggle id="st-http-modpacks" checked={!!s.data?.allowHttpModpacks} disabled={!s.data} onChange={(on) => set({ allowHttpModpacks: on })} />} />
    </Group>
  );
}

/** Steam libraries Kiln does not find on its own. */
function SteamGroup() {
  const t = useT();
  const s = useSettings();
  const set = useSetSettings();
  const [root, setRoot] = useState("");
  const roots = s.data?.steamRoots ?? [];
  return (
    <Group id="steam" items={["steamRoots"]}>
      <SettingRow id="steamRoots" htmlFor="st-steam-root" wide
        control={(
          <div className="k-stack" style={{ gap: 6 }}>
            {roots.map((r) => (
              <div key={r} className="k-input-group">
                <span className="k-mono k-ellipsis" style={{ flex: 1 }}>{r}</span>
                <IconButton label={t("common.remove")} size="s" icon={<X />} onClick={() => set({ steamRoots: roots.filter((x) => x !== r) })} />
              </div>
            ))}
            <div className="k-input-group">
              <div style={{ flex: 1, minWidth: 0 }}><PathPicker id="st-steam-root" value={root} onChange={setRoot} directory placeholder={WIN ? "D:\\SteamLibrary" : "/mnt/games/SteamLibrary"} /></div>
              <IconButton label={t("common.add")} variant="secondary" icon={<Plus />} disabled={!root.trim() || !s.data} onClick={() => { set({ steamRoots: [...roots, root.trim()] }); setRoot(""); }} />
            </div>
          </div>
        )} />
    </Group>
  );
}

/** The rest of the selected game's kiln/kiln.json: the entries no other group has (language: General; look, size, chat,
 *  outlines: Game & Kiln; graphics: Graphics; shroudforge.*: the ShroudForge card), any key a newer core adds included. */
function KilnFileGroup() {
  const t = useT();
  const { game } = useGame();
  const status = useGameStatus(game?.id);
  const k = useKilnJson(game?.id);
  const v = useSettingsView();
  if (!game) return null;
  const schema = k.doc.data?.schema;
  const ids = (schema?.entries ?? []).filter((e) => kilnGroupOf(e.path) === "kilnjson").map((e) => `kj:${e.path}`);
  const only = v.hits ? new Set([...v.hits].filter((h) => h.startsWith("kj:"))) : null;
  const installed = status.data?.runtime.installed;
  return (
    <Group id="kilnjson" items={ids} hint={rich("settings.kilnJson.sub", { file: <span className="k-mono">kiln/kiln.json</span> })}>
      {status.data && !installed ? <EmptyState title={t("settings.kilnJson.notInstalled")} hint={t("settings.kilnJson.notInstalledHint")} /> :
        k.doc.isLoading ? <Skeleton lines={5} /> : k.doc.error ? <EmptyState title={t("settings.kilnJson.unavailable")} hint={errorText(k.doc.error)} /> : schema && (
          <SchemaForm clamp schema={kilnSchema(schema, "kilnjson", only)} values={k.doc.data!.values} errors={k.doc.data!.errors} disabled={k.busy} labels={kilnLabels()}
            onChange={(path, value) => k.set.mutate({ path, value })} onReset={(path) => k.reset.mutate(path)} />
        )}
      {k.doc.data && !v.hits && <span className="k-hint">{t("settings.kilnJson.overrides")}</span>}
    </Group>
  );
}

/** The art the launcher makes from the player's own game, on its own: status only (components/GameArt.tsx,
 *  docs/kiln/launcher/gameart.md). */
function ArtGroup() {
  const t = useT();
  const shown = useShown("art");
  const st = useAssetsStatus();
  const editorTask = useExtractTask(st.data, "editor");
  const adminTask = useExtractTask(st.data, "admin");
  const d = st.data;
  const e = d?.editor;
  const a = d?.admin;
  if (!shown) return null;
  const when = (at?: string | null) => (at ? fmtAgo(at) : null);
  // a count from the art's counts, in words ("1,234 icons"); null when there is none
  const n = (c: Record<string, number> | undefined, k: "icons" | "ui" | "tiles") => {
    const count = c?.[k];
    if (!count) return null;
    return k === "icons" ? t("settings.art.icons", { count }) : k === "ui" ? t("settings.art.ui", { count }) : t("settings.art.tiles", { count });
  };
  const join = (...xs: (string | null | false | undefined)[]) => xs.filter(Boolean).join(" · ");
  const badge = (present: boolean, current: boolean | null | undefined, task: unknown) =>
    task ? <Badge tone="ember">{t("settings.art.preparing")}</Badge> : !present ? <Badge tone="idle">{t("settings.art.missing")}</Badge>
      : current === false ? <Badge tone="warn">{t("settings.art.updating")}</Badge> : <Badge tone="ok">{t("settings.art.ready")}</Badge>;
  const row = (label: string, b: ReactNode, lines: string[]) => (
    <div className="k-list-item">
      <span className="k-stack" style={{ gap: 1, flex: 1 }}>
        <span className="k-row" style={{ gap: 8 }}><span className="k-item-title">{label}</span>{b}</span>
        {lines.filter(Boolean).map((l) => <span key={l} className="k-item-sub">{l}</span>)}
      </span>
    </div>
  );
  return (
    <Group id="art" items={["art"]} hint={t("settings.art.sub")}>
      {st.error ? <EmptyState title={t("settings.art.unavailable")} hint={errorText(st.error)} /> : !d || !e || !a ? <Skeleton /> : (
        <div className="k-list k-st-items" data-setting="art">
          {row(t("settings.art.editor"), badge(e.present, e.current, editorTask), [
            !e.canExtract && !e.present ? t("settings.art.noClient") : join(
              e.source === "game" ? t("settings.art.fromGame", { build: e.build ?? "?" }) : e.source === "copy" ? t("settings.art.fromFolder") : null,
              n(e.counts, "icons"), n(e.counts, "ui"), e.bytes ? fmtBytes(e.bytes) : null, when(e.extracted)),
            e.modded ? join(e.modded.icons ? t("settings.art.modItemsIcons", { items: e.modded.items, icons: e.modded.icons }) : t("settings.art.modItems", { items: e.modded.items }),
              e.modded.mods.length ? e.modded.mods.join(", ") : t("settings.art.noMods"), when(e.modded.generated)) : "",
          ])}
          {row(t("settings.art.admin"), badge(a.map, a.current, adminTask), [
            !a.canExtract && !a.map ? t("settings.art.noClient") : join(
              a.source === "game" ? t("settings.art.reliefFromGame", { build: a.build ?? "?" }) : a.map ? t("settings.art.fromLocalFolder") : null,
              n(a.counts, "tiles"), a.counts?.markers ? t("settings.art.markers", { count: a.counts.markers }) : a.markers ? t("settings.art.markersLocal") : a.map ? t("settings.art.noMarkers") : null,
              a.bytes ? fmtBytes(a.bytes) : null, when(a.extracted)),
          ])}
        </div>
      )}
    </Group>
  );
}

/** Dev builds of the desktop app: read-only screens show sample data where the backend is not done. */
function DeveloperGroup() {
  const [on, setOn] = useState(fallbackEnabled());
  if (!api.isTauri()) return null;
  return (
    <Group id="developer" items={["devMock"]}>
      <SettingRow id="devMock" htmlFor="st-dev-mock"
        control={<Toggle id="st-dev-mock" checked={on} onChange={(v) => { localStorage.setItem("kiln.mockFallback", v ? "1" : "0"); setOn(v); }} />} />
    </Group>
  );
}
