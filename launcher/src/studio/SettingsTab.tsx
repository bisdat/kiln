// Settings (DESIGN 3.2): overrides are captured from the pack's profile, never written by hand. "Capture from my current
// settings" lists each settings file with the keys that differ from the mod's defaults (its .schema/ snapshot; a file
// without one offers every key); hotkeys and key bindings are personal and left out unless ticked; anything that looks
// like a secret can never travel. Chosen keys become merge patches / INI keys, so players keep their own keys and
// updates merge per key. The server's game settings (gameSettings only: never a name, password or ports) are taken
// from one of the launcher's servers the same way.
import { useEffect, useState } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { GameId, Json, PackSide, Studio, StudioCaptureChoice, StudioCaptureFile } from "../api/commands";
import { useT } from "../i18n";
import { Badge, Button, Select, Spinner, toast } from "../ui";
import { sk } from "./state";

const show = (v: Json | undefined) => (v === undefined ? "" : typeof v === "string" ? JSON.stringify(v) : JSON.stringify(v));

export default function SettingsTab({ studio: st, gameId }: { studio: Studio; gameId: GameId | null }) {
  const t = useT();
  const qc = useQueryClient();
  const id = st.source.id;
  const [files, setFiles] = useState<StudioCaptureFile[] | null>(null);
  const [busy, setBusy] = useState(false);
  const [dirty, setDirty] = useState(false);
  const capture = async () => {
    setBusy(true);
    try { setFiles(await api.studioCapture(id, gameId)); setDirty(false); } catch (e) { toast.error(e, t("studio.settings.captureFailed")); } finally { setBusy(false); }
  };
  // what the pack carries now shows at once (without copying the game's settings back)
  useEffect(() => { void api.studioCapture(id, null).then(setFiles).catch(() => setFiles([])); }, [id]);
  const edit = (path: string, f: (x: StudioCaptureFile) => StudioCaptureFile) => { setFiles((all) => all?.map((x) => (x.path === path ? f(x) : x)) ?? null); setDirty(true); };
  const apply = async () => {
    if (!files) return;
    const choices: StudioCaptureChoice[] = files.filter((f) => !f.refused).flatMap((f) => {
      if (f.whole) return [{ path: f.path, side: f.side, keys: [], whole: true }];
      const keys = f.keys.filter((k) => k.chosen && !k.secret).map((k) => k.key);
      return keys.length ? [{ path: f.path, side: f.side, keys }] : [];
    });
    setBusy(true);
    try {
      const next = await api.studioCaptureSet(id, choices);
      qc.setQueryData(sk.get(id), next);
      setDirty(false);
      toast.ok(t("studio.settings.saved", { count: choices.length }));
    } catch (e) { toast.error(e, t("studio.settings.saveFailed")); } finally { setBusy(false); }
  };
  const chosen = files?.reduce((n, f) => n + (f.whole ? 1 : f.keys.filter((k) => k.chosen).length), 0) ?? 0;
  return (
    <section className="kst-section" data-testid="studio-settings">
      <p className="kst-p">{t("studio.settings.lead")}</p>
      <div className="kst-row">
        <Button variant="primary" loading={busy} onClick={() => void capture()} data-testid="studio-capture">{t("studio.settings.capture")}</Button>
        <Button disabled={!dirty || busy} onClick={() => void apply()} data-testid="studio-capture-save">{t("studio.settings.save", { count: chosen })}</Button>
      </div>
      {!files ? <Spinner /> : files.length === 0 ? <p className="kst-p kst-dim">{t("studio.settings.none")}</p> : (
        <ul className="kst-files" aria-busy={busy || undefined}>
          {files.map((f) => (
            <li key={f.path} className="kst-file" data-testid={"studio-file-" + f.path}>
              <div className="kst-row kst-between">
                <span><span className="kst-mono">{f.path}</span>{" "}
                  <span className="kst-dim">{f.refused ? t("studio.settings.refused", { why: f.refused })
                    : f.mode === "replace" ? t("studio.settings.wholeOnly")
                    : f.defaults ? t("studio.settings.differs", { count: f.keys.length }) : t("studio.settings.noDefaults", { count: f.keys.length })}</span>
                </span>
                {!f.refused && (
                  <span className="kst-row">
                    <Select value={f.side} aria-label={t("studio.settings.sideAria", { file: f.path })} onChange={(v) => edit(f.path, (x) => ({ ...x, side: v as PackSide }))}
                      options={[{ value: "both", label: t("studio.mods.both") }, { value: "client", label: t("studio.mods.client") }, { value: "server", label: t("studio.mods.server") }]} />
                    {(f.mode === "replace" || !f.defaults) && (
                      <label className="kst-tick"><input type="checkbox" className="k-check" checked={f.whole} data-testid={"studio-whole-" + f.path}
                        disabled={busy || f.keys.some((k) => k.secret)}
                        onChange={(e) => edit(f.path, (x) => ({ ...x, whole: e.target.checked }))} />{t("studio.settings.whole")}</label>
                    )}
                  </span>
                )}
              </div>
              {!f.refused && f.keys.length > 0 && (
                <ul className="kst-keys">
                  {f.keys.map((k) => (
                    <li key={k.key}>
                      <label className={"kst-tick" + (k.secret ? " kst-off" : "")} title={k.secret}>
                        <input type="checkbox" className="k-check" checked={k.chosen || f.whole} disabled={busy || !!k.secret || f.whole} data-testid={"studio-key-" + f.path + "#" + k.key}
                          onChange={(e) => edit(f.path, (x) => ({ ...x, keys: x.keys.map((y) => (y.key === k.key ? { ...y, chosen: e.target.checked } : y)) }))} />
                        <span className="kst-mono">{k.key}</span> <span>{show(k.value)}</span>
                        {k.default !== undefined && <span className="kst-dim">{t("studio.settings.default", { value: show(k.default) })}</span>}
                        {k.personal && <Badge tone="warn">{t("studio.settings.personal")}</Badge>}
                        {k.secret && <Badge tone="err">{t("studio.settings.secret")}</Badge>}
                      </label>
                    </li>
                  ))}
                </ul>
              )}
            </li>
          ))}
        </ul>
      )}
      <ServerSettings studio={st} />
    </section>
  );
}

/** The server's game settings, taken from one of the launcher's servers (gameSettings only). */
function ServerSettings({ studio: st }: { studio: Studio }) {
  const t = useT();
  const qc = useQueryClient();
  const servers = useQuery({ queryKey: ["server_list"], queryFn: api.serverList, retry: false });
  const [server, setServer] = useState("");
  const [values, setValues] = useState<Record<string, Json> | null>(null);
  const [picked, setPicked] = useState<Record<string, boolean>>({});
  const current = (st.source.serverSettings?.gameSettings ?? {}) as Record<string, Json>;
  const local = (servers.data ?? []).filter((s) => s.kind !== "remote");
  const take = async () => {
    try {
      const s = await api.serverSettingsGet(server || local[0]?.id);
      const gs = ((s.values as Record<string, Json> | null)?.gameSettings ?? {}) as Record<string, Json>;
      setValues(gs);
      setPicked(Object.fromEntries(Object.keys(gs).map((k) => [k, k in current])));
    } catch (e) { toast.error(e, t("studio.server.takeFailed")); }
  };
  const save = async (gs: Record<string, Json> | null) => {
    try {
      const next = await api.studioSave(st.source.id, { ...st.source, serverSettings: gs && Object.keys(gs).length ? { gameSettings: gs } : null });
      qc.setQueryData(sk.get(st.source.id), next);
      setValues(null);
      toast.ok(t("studio.server.saved"));
    } catch (e) { toast.error(e); }
  };
  return (
    <div className="kst-server" data-testid="studio-server-settings">
      <h3 className="kst-h3">{t("studio.server.title")}</h3>
      <p className="kst-p kst-dim">{t("studio.server.lead")}</p>
      {Object.keys(current).length > 0 && (
        <ul className="kst-keys" data-testid="studio-server-current">
          {Object.entries(current).map(([k, v]) => <li key={k}><span className="kst-mono">{k}</span> {show(v)}</li>)}
          <li><Button size="s" variant="ghost" onClick={() => void save(null)}>{t("studio.server.clear")}</Button></li>
        </ul>
      )}
      {local.length === 0 ? <p className="kst-p kst-dim">{t("studio.server.none")}</p> : (
        <div className="kst-row">
          <Select value={server || local[0].id} onChange={setServer} aria-label={t("studio.server.pick")} options={local.map((s) => ({ value: s.id, label: s.name }))} />
          <Button onClick={() => void take()} data-testid="studio-server-take">{t("studio.server.take")}</Button>
        </div>
      )}
      {values && (
        <div className="kst-file">
          <ul className="kst-keys">
            {Object.entries(values).map(([k, v]) => (
              <li key={k}><label className="kst-tick"><input type="checkbox" className="k-check" checked={!!picked[k]} data-testid={"studio-gs-" + k}
                onChange={(e) => setPicked((p) => ({ ...p, [k]: e.target.checked }))} /><span className="kst-mono">{k}</span> {show(v)}</label></li>
            ))}
          </ul>
          <Button variant="primary" onClick={() => void save(Object.fromEntries(Object.entries(values).filter(([k]) => picked[k])))} data-testid="studio-server-save">{t("studio.server.use")}</Button>
        </div>
      )}
    </div>
  );
}
