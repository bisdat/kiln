// What the desktop shell mounts once for Crucible: the pack preview and the update screen (opened from anywhere through
// useCrucibleUi), kiln://crucible links (the first one opens the preview, as nxm links open the catalog's install), the
// share that waited for a Discord sign-in, and the sign-in dialog itself when the Community page is hidden.
import { useQueryClient } from "@tanstack/react-query";
import { useEffect } from "react";
import * as api from "../api/commands";
import { SignInDialog } from "../community/SignIn";
import { useCommunityUi, useShown } from "../community/state";
import { PreviewModal } from "./Preview";
import { startShare } from "./Share";
import { UpdateModal } from "./Update";
import { crucibleTarget } from "./code";
import { crk, readClipboard, useCrucibleOn, useCrucibleStatus, useCrucibleUi, useShareRun, useStatusFollowsSignIn } from "./state";
import "./crucible.css";

/** Takes the kiln://crucible links that arrive and hands the first to `open`. */
export function useCrucibleLinks(open: (target: string) => void) {
  const on = useCrucibleOn();
  useEffect(() => {
    if (!on) return;
    let live = true, off: (() => void) | undefined;
    const take = async () => {
      let links: string[] = [];
      try { links = await api.crucibleLinks(); } catch { return; }
      if (live && links.length) open(links[0]);
    };
    api.onKilnEvent("kiln://crucible-link", () => void take()).then((f) => { if (!live) { f(); return; } off = f; void take(); }).catch(() => { if (live) void take(); });
    return () => { live = false; off?.(); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [on]);
}

/** "Come back after installing Kiln" (DESIGN 1.3): once per start, the code the download carried (the installer's note,
 *  the AppImage's name), else at the very first start the pack link the pack page copied to the clipboard, opens the
 *  preview ("Ready to install ..."). Nothing installs without the preview's own clicks. */
export function useCrucibleResume(open: (target: string) => void) {
  const on = useCrucibleOn();
  useEffect(() => {
    if (!on) return;
    let live = true;
    void (async () => {
      let r: api.CrucibleResume;
      try { r = await api.crucibleResume(); } catch { return; }
      let target = r.code;
      if (!target && r.firstStart) target = crucibleTarget(await readClipboard());
      if (live && target) open(target);
    })();
    return () => { live = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [on]);
}

/** A share that asked for the sign-in starts once the player is signed in. */
export function useShareAfterSignIn() {
  const qc = useQueryClient();
  const status = useCrucibleStatus();
  const pending = useShareRun((s) => Object.values(s.runs).find((r) => r.pendingSignIn));
  useEffect(() => {
    if (!pending) return;
    if (status.data?.signedIn) { useShareRun.getState().set({ ...pending, pendingSignIn: false, error: null }); void startShare(qc, pending.profileId); return; }
    const timer = setInterval(() => void qc.invalidateQueries({ queryKey: crk.status }), 1500);
    return () => clearInterval(timer);
  }, [pending, status.data?.signedIn, qc]);
}

export function CrucibleHost() {
  useStatusFollowsSignIn();
  useShareAfterSignIn();
  const on = useCrucibleOn();
  const preview = useCrucibleUi((s) => s.preview);
  const update = useCrucibleUi((s) => s.update);
  const openPreview = useCrucibleUi((s) => s.openPreview);
  useCrucibleLinks((target) => openPreview({ kind: "crucible", target }));
  useCrucibleResume((target) => openPreview({ kind: "crucible", target, resume: true }));
  const shown = useShown();
  const signIn = useCommunityUi((s) => s.signIn);
  return (
    <>
      {preview && (on || preview.kind === "file") && <PreviewModal key={preview.kind === "file" ? preview.path : preview.target} source={preview} />}
      {update && on && <UpdateModal key={update} profileId={update} />}
      {/* the Community host draws the sign-in while the page is shown; sharing needs it either way */}
      {signIn && !shown && <SignInDialog resumeSend={false} />}
    </>
  );
}
