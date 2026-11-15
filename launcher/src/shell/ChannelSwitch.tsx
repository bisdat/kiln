// (dev.29) Switching between the release and the beta (test builds) with one click, both ways (selfupdate, update_switch).
// Settings > Updates > Channel and the Beta mark in the header open it; the Deck has its own (deck/screens/Settings.tsx).
//   to-beta  a release build: the newest test build is installed now, then Restart now
//   back     a test build: the newest release is installed now, also when it is older, then Restart now; with a
//            "before beta" copy, the restart can put the profiles and settings from before the beta back first
// Both say what happens to worlds and multiplayer. An install that cannot replace itself (a .deb, a development build)
// gets the download page instead; the channel is saved either way, so the next update follows it.
import { useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import { create } from "zustand";
import * as api from "../api/commands";
import type { UpdateChannel, UpdateStatus } from "../api/commands";
import { RELEASE_CHANNEL, buildLabel, restoreWords, switchKind, switchWords } from "../api/channel";
import { useUpdate } from "../api/update";
import { saveSettings } from "../deck/mode";
import { Button, Modal, ProgressBar, Toggle, toast, waitTask } from "../ui";

interface SwitchStore {
  target: UpdateChannel | null;
  show: (target: UpdateChannel) => void;
  close: () => void;
}

export const useChannelSwitch = create<SwitchStore>((set) => ({
  target: null,
  show: (target) => set({ target }),
  close: () => set({ target: null }),
}));

/** Runs a switch: saves the channel, then (an install that replaces itself) downloads and installs the build. Shared by
 *  the desktop dialog and the Deck's. Resolves to the status after it, or null when it did not run. */
export async function runSwitch(qc: ReturnType<typeof useQueryClient>, s: UpdateStatus, target: UpdateChannel, restore: boolean): Promise<UpdateStatus | null> {
  const saved = await saveSettings(qc, () => ({ updateChannel: target }));
  if (!saved) return null;
  if (!s.install.inPlace) {
    await api.appOpenUrl(s.page).catch(() => toast.error({ code: "internal", message: "Could not open the page" }));
    return null;
  }
  try {
    return await waitTask<UpdateStatus>(await api.updateSwitch(target, restore));
  } catch (e) {
    const err = e as api.KilnError;
    if (err?.code === "not_found") toast.info("Nothing to switch to", err.message);
    else toast.error(err ?? { code: "internal", message: "Switch failed" }, "Switch failed");
    return null;
  }
}

/** Mounted once in the shell. */
export function ChannelSwitchDialog() {
  const target = useChannelSwitch((x) => x.target);
  const close = useChannelSwitch((x) => x.close);
  const s = useUpdate();
  if (!target || !s) return null;
  const kind = switchKind(s, target);
  if (!kind) return null;
  return <SwitchDialog s={s} target={target} kind={kind} onClose={close} />;
}

function SwitchDialog({ s, target, kind, onClose }: { s: UpdateStatus; target: UpdateChannel; kind: "to-beta" | "back"; onClose: () => void }) {
  const qc = useQueryClient();
  const w = switchWords(kind);
  const [busy, setBusy] = useState(false);
  const [restore, setRestore] = useState(false);
  const [restarting, setRestarting] = useState(false);
  const ready = s.phase === "ready" && s.switching === target && s.ready;
  const downloading = s.phase === "downloading" && s.switching === target;
  const go = async () => {
    setBusy(true);
    try { await runSwitch(qc, s, target, kind === "back" && restore && !!s.beforeBeta); } finally { setBusy(false); }
  };
  const restart = async () => {
    setRestarting(true);
    try { await api.updateRestart(); } catch { toast.error({ code: "internal", message: "Restart failed" }); setRestarting(false); }
  };
  const r = s.beforeBeta ? restoreWords(s.beforeBeta.taken) : null;
  const footer = ready ? (
    <><Button variant="ghost" onClick={onClose}>Later</Button><Button variant="primary" loading={restarting} onClick={restart}>Restart now</Button></>
  ) : (
    <><Button variant="ghost" onClick={onClose} disabled={busy || downloading}>Cancel</Button>
      <Button variant="primary" loading={busy || downloading} disabled={s.phase === "ready" || (s.phase === "downloading" && !downloading)} onClick={go}>
        {s.install.inPlace ? w.confirm : "Open the download page"}
      </Button></>
  );
  return (
    <Modal open onClose={onClose} title={ready ? `Ready: ${buildLabel(s.ready!.version)}` : w.title} width={520} locked={busy || downloading} footer={footer}>
      <div style={{ display: "flex", flexDirection: "column", gap: 10 }} data-testid={`switch-${kind}`}>
        {ready ? (
          <span>{kind === "back" ? "The release is in place." : "The test build is in place."} Restart now to run it{kind === "back" && restore && s.beforeBeta ? "; your profiles and settings from before the beta go back first" : ""}.</span>
        ) : (
          <>
            {kind === "back" && s.beta ? <span className="k-hint">This is test build {s.beta} of {s.current}.</span> : null}
            {w.say.map((line) => <span key={line}>{line}</span>)}
            {!s.install.inPlace && <span className="k-hint">{s.install.reason} The download page has the build; the launcher follows its channel from now on.</span>}
            {kind === "back" && r && s.install.inPlace && (
              <Toggle checked={restore} onChange={setRestore} label={r.label} hint={r.hint} disabled={busy || downloading} />
            )}
            {downloading && <ProgressBar value={s.progress?.total ? s.progress.received / s.progress.total : null} />}
          </>
        )}
      </div>
    </Modal>
  );
}

/** The header's Beta mark (a test build is running): the beta number, and one click back to the release. */
export function BetaMark() {
  const s = useUpdate();
  const show = useChannelSwitch((x) => x.show);
  if (!s || s.buildChannel !== "beta") return null;
  return (
    <button type="button" className="k-beta-flag" data-testid="beta-mark" onClick={() => show(RELEASE_CHANNEL)}
      title={`Test build ${s.beta ?? ""} of ${s.current}. Click to switch back to the release.`} aria-label={`Beta ${s.beta ?? ""}: switch back to the release`}>
      Beta {s.beta ?? ""}
    </button>
  );
}
