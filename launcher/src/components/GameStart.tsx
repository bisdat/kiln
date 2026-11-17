// How Kiln starts a game install (dev.29.17, kiln_manage::direct; docs/kiln/launcher/custom-game-folders.md): through
// Steam (steam://rungameid, the default for Steam's own copy) or directly (the launcher starts this folder's
// enshrouded.exe: the default for a folder no Steam library knows). On Linux a direct start runs under a Proton or Wine
// the player picks, in Kiln's own prefix for the install unless the player picks one. Also the line for a game build
// Kiln does not know, used on Play and in Settings' game list.
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import * as api from "../api/commands";
import type { BuildCheck, GameInstall, StartMode, StartSettings } from "../api/commands";
import { qk } from "../api/hooks";
import { useT } from "../i18n";
import { Banner, Button, Field, Modal, PathPicker, Segmented, Select, toast } from "../ui";

/** The player docs on custom folders (Steam, unknown builds), linked from the launcher. */
export const CUSTOM_FOLDERS_DOCS = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/launcher/custom-game-folders.md";

export function openStartDocs(): void {
  api.appOpenUrl(CUSTOM_FOLDERS_DOCS).catch((e) => toast.error(e));
}

/** "Through Steam" / "Directly": the short word for an install's start. */
export function useStartWord(): (mode: StartMode | undefined) => string {
  const t = useT();
  return (mode) => (mode === "direct" ? t("gameStart.word.direct") : t("gameStart.word.steam"));
}

export function useStartSetup(gameId: string | null | undefined, enabled = true) {
  return useQuery({ queryKey: qk.startSetup(gameId ?? ""), queryFn: () => api.gameStartGet(gameId!), enabled: !!gameId && enabled, refetchInterval: 15000 });
}

const AUTO = "";
const CUSTOM = "\u0000custom";

/** The start of one install: the mode, and on Linux the Proton or Wine and the prefix of a direct start. `added`: shown
 *  right after "Add a game folder" (the folder was just recorded with Kiln's default). */
export function StartModal({ game, open, onClose, added }: { game: GameInstall; open: boolean; onClose: () => void; added?: boolean }) {
  const t = useT();
  const qc = useQueryClient();
  const setup = useStartSetup(game.id, open);
  const s = setup.data;
  const [mode, setMode] = useState<StartMode>("steam");
  const [runner, setRunner] = useState(AUTO);
  const [customRunner, setCustomRunner] = useState("");
  const [ownPrefix, setOwnPrefix] = useState(true);
  const [prefix, setPrefix] = useState("");
  // the form starts from what the install has whenever the dialog opens
  useEffect(() => {
    if (!open || !s) return;
    setMode(s.mode);
    const known = s.settings.runner && s.runners.some((r) => r.path === s.settings.runner);
    setRunner(!s.settings.runner ? AUTO : known ? s.settings.runner : CUSTOM);
    setCustomRunner(known ? "" : s.settings.runner ?? "");
    setOwnPrefix(!s.settings.prefix);
    setPrefix(s.settings.prefix ?? "");
  }, [open, s]);
  const settings = (): StartSettings => {
    const out: StartSettings = {};
    // the mode is written only when it differs from Kiln's default for this folder
    if (mode !== (s?.custom ? "direct" : "steam")) out.mode = mode;
    if (mode === "direct" && !s?.windows) {
      const r = runner === CUSTOM ? customRunner.trim() : runner;
      if (r) out.runner = r;
      if (!ownPrefix && prefix.trim()) out.prefix = prefix.trim();
    }
    return out;
  };
  const save = useMutation({
    mutationFn: () => api.gameStartSet(game.id, settings()),
    onSuccess: () => {
      for (const k of [qk.games, qk.gameStatus(game.id), qk.startSetup(game.id)]) void qc.invalidateQueries({ queryKey: k });
      toast.ok(mode === "direct" ? t("gameStart.savedDirect") : t("gameStart.savedSteam"));
      onClose();
    },
    onError: (e) => toast.error(e),
  });
  const auto = s?.defaultRunner ? t("gameStart.runnerAuto", { name: s.defaultRunner.label }) : t("gameStart.runnerNone");
  const runnerOptions = [
    { value: AUTO, label: auto },
    ...(s?.runners ?? []).map((r) => ({ value: r.path, label: `${r.label} · ${r.source === "steam" ? t("gameStart.fromSteam") : r.source === "compat" ? t("gameStart.fromCompat") : r.kind === "wine" ? "Wine" : t("gameStart.fromPath")}` })),
    { value: CUSTOM, label: t("gameStart.runnerOther") },
  ];
  const cmd = s?.command;
  const preview = cmd ? [...cmd.env.map(([k, v]) => `${k}=${quote(v)}`), quote(cmd.program), ...cmd.args.map(quote)].join(" ") : null;
  return (
    <Modal open={open} onClose={onClose} title={added ? t("gameStart.titleAdded") : t("gameStart.title")}
      footer={<>
        <button type="button" className="k-link" style={{ marginRight: "auto" }} onClick={openStartDocs}>{t("gameStart.docs")}</button>
        <Button variant="ghost" onClick={onClose}>{added ? t("gameStart.later") : t("common.cancel")}</Button>
        <Button variant="primary" disabled={!s || (mode === "direct" && runner === CUSTOM && !customRunner.trim()) || (mode === "direct" && !ownPrefix && !prefix.trim())}
          loading={save.isPending} onClick={() => save.mutate()}>{t("common.save")}</Button>
      </>}>
      <div className="k-stack" style={{ gap: 12 }} data-testid="game-start">
        {s?.custom && <span className="k-hint">{t("gameStart.customIntro")}</span>}
        <Field label={t("gameStart.how")}>
          <Segmented<StartMode> aria-label={t("gameStart.how")} value={mode} onChange={setMode}
            options={[{ id: "steam", label: t("gameStart.steam") }, { id: "direct", label: t("gameStart.direct") }]} />
        </Field>
        <span className="k-hint">{mode === "steam" ? (s?.custom ? t("gameStart.steamCustomHint") : t("gameStart.steamHint")) : t("gameStart.directHint")}</span>
        {mode === "direct" && (
          <>
            <span className="k-hint" data-testid="steam-required">{t("gameStart.steamRequired")}</span>
            {s && !s.steamRunning && <Banner tone="warn" title={t("gameStart.steamNotRunning")}>{t("gameStart.steamNotRunningText")}</Banner>}
            {s && !s.windows && (
              <>
                <Field label={t("gameStart.runner")} hint={t("gameStart.runnerHint")} htmlFor="start-runner">
                  <Select id="start-runner" value={runner} onChange={setRunner} options={runnerOptions} />
                </Field>
                {runner === CUSTOM && (
                  <Field label={t("gameStart.runnerPath")} hint={t("gameStart.runnerPathHint")} htmlFor="start-runner-path">
                    <PathPicker id="start-runner-path" value={customRunner} onChange={setCustomRunner} placeholder="/…/GE-Proton10-25" />
                  </Field>
                )}
                <Field label={t("gameStart.prefix")} hint={ownPrefix ? t("gameStart.prefixOwnHint", { path: s.ownPrefix ?? "" }) : t("gameStart.prefixPickHint")} htmlFor="start-prefix">
                  <Segmented<"own" | "pick"> aria-label={t("gameStart.prefix")} value={ownPrefix ? "own" : "pick"} onChange={(v) => setOwnPrefix(v === "own")}
                    options={[{ id: "own", label: t("gameStart.prefixOwn") }, { id: "pick", label: t("gameStart.prefixPick") }]} />
                </Field>
                {!ownPrefix && <PathPicker id="start-prefix" value={prefix} onChange={setPrefix} directory placeholder="/…/compatdata/1203620" />}
              </>
            )}
            {s?.problem && <Banner tone="err" title={t("gameStart.cannotStart")}>{s.problem}</Banner>}
            {preview && mode === s?.mode && (
              <Field label={s.windows ? t("gameStart.commandWindows") : t("gameStart.command")}>
                <span className="k-mono k-hint" style={{ overflowWrap: "anywhere" }} data-testid="start-command">{preview}</span>
              </Field>
            )}
          </>
        )}
      </div>
    </Modal>
  );
}

function quote(v: string): string {
  return /^[\w./:=,@+-]*$/.test(v) && v !== "" ? v : `"${v.replace(/(["\\$`])/g, "\\$1")}"`;
}

/** The plain sentence for a game build Kiln does not know, with the docs link; nothing for a known build. */
export function UnknownBuild({ build, compact }: { build: BuildCheck | null | undefined; compact?: boolean }) {
  const t = useT();
  if (!build || build.known) return null;
  const text = t("gameStart.unknownBuild", { hash: build.short });
  if (compact) {
    return (
      <span className="k-item-sub" style={{ color: "var(--k-warn)" }} data-testid="unknown-build">
        {text} <button type="button" className="k-link" onClick={openStartDocs}>{t("gameStart.readMore")}</button>
      </span>
    );
  }
  return (
    <Banner tone="warn" title={t("gameStart.unknownBuildTitle")} actions={<Button size="s" onClick={openStartDocs}>{t("gameStart.readMore")}</Button>}>
      <span data-testid="unknown-build">{text}</span>
    </Banner>
  );
}
