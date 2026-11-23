// Launcher updates: the pieces of Settings' "Launcher updates" group (automatic checks, channel, Check now:
// pages/settings/sections/Online.tsx) and About's line (the state and Check now). The state comes from the shell's update
// feed (api/update.ts). Short words only; details are in launcher.log.
// (dev.29) Channel: Release or Beta (test builds) (api/channel.ts). Choosing the other one on a build of the one before
// is a switch (shell/ChannelSwitch.tsx): the dialog says what happens, installs that build now and restarts. A test
// build says so here, with "Switch back to the release".
// The Nexus Mods build (api/distribution.ts) has none of that: one line, "New versions: Nexus Mods", whose link opens
// the Kiln page there, and where test builds are (the download page, which has launchers that update themselves); no
// update command is called.
import { useState } from "react";
import * as api from "../api/commands";
import type { UpdateStatus } from "../api/commands";
import { DIRECT_DOWNLOAD_PAGE, NEXUS_PAGE } from "../api/distribution";
import { useDistribution } from "../api/hooks";
import { useUpdate, useUpdateStore } from "../api/update";
import { t, useT } from "../i18n";
import { toast } from "../ui";

/** Where updates stand, in a few words. */
export function updateWords(s: UpdateStatus): string {
  switch (s.phase) {
    case "checking": return t("updateSettings.checking");
    case "upToDate": return t("updateSettings.upToDate");
    case "available": return t("updateSettings.available", { version: String(s.available?.version) });
    case "skipped": return t("updateSettings.skipped", { version: String(s.available?.version) });
    case "failed": return t("updateSettings.failed");
    case "downloading": return t("updateSettings.updating");
    case "ready": return t("updateSettings.ready");
    default: return s.auto ? t("updateSettings.notChecked") : t("updateSettings.off");
  }
}

/** Check now: asks once, opens the notice when an update is found, says the rest in a toast. */
export function useCheckNow() {
  const [busy, setBusy] = useState(false);
  const run = async () => {
    setBusy(true);
    try {
      const s = await api.updateCheck();
      if (s.phase === "available") useUpdateStore.getState().setOpen(true);
      else if (s.phase === "upToDate") toast.ok(t("updateSettings.upToDate"));
      else if (s.phase === "failed") toast.error({ code: "internal", message: t("updateSettings.failed") });
    } catch {
      toast.error({ code: "internal", message: t("updateSettings.failed") });
    } finally {
      setBusy(false);
    }
  };
  return { busy, run };
}

/** The Nexus build's whole update UI: where new versions are, as a link (opened in the browser, nothing fetched). */
export function NexusVersionsLink() {
  return (
    <button type="button" className="k-link" data-testid="nexus-page" title={NEXUS_PAGE}
      onClick={() => api.appOpenUrl(NEXUS_PAGE).catch(() => toast.error({ code: "internal", message: t("updateSettings.openFailed") }))}>
      Nexus Mods
    </button>
  );
}

/** The Nexus build's line about test builds: they come from the download page (launchers that update themselves). */
export function NexusTestBuildsLink() {
  const t = useT();
  return (
    <button type="button" className="k-link" data-testid="nexus-test-builds" title={DIRECT_DOWNLOAD_PAGE}
      onClick={() => api.appOpenUrl(DIRECT_DOWNLOAD_PAGE).catch(() => toast.error({ code: "internal", message: t("updateSettings.openFailed") }))}>
      {t("updateSettings.downloadPage")}
    </button>
  );
}

/** About: the state and Check now (direct builds); the Nexus build's row is "New versions: Nexus Mods" (AboutPage). */
export function UpdateAboutLine() {
  return useDistribution() === "direct" ? <SelfUpdateAboutLine /> : null;
}

function SelfUpdateAboutLine() {
  const t = useT();
  const s = useUpdate();
  const check = useCheckNow();
  if (!s) return <span>-</span>;
  return (
    <span>
      {updateWords(s)}{" · "}
      <button type="button" className="k-link" disabled={check.busy || s.phase === "checking"} onClick={check.run}>{t("updateSettings.checkNow")}</button>
    </span>
  );
}
