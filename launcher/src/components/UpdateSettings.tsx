// Launcher updates: the pieces of Settings' "Launcher updates" group (automatic checks, channel, Check now:
// pages/settings/sections/Online.tsx) and About's line (the state and Check now). The state comes from the shell's update
// feed (api/update.ts). Short words for the summaries (updateWords); About's and Settings' line (checkLine, also the
// Deck's row) says what the last check found and when, in local time, and why it failed (each source's answer), so a
// player sees that Check now checked even when nothing changed. Details are in launcher.log.
// (dev.29) Channel: Release or Beta (test builds) (api/channel.ts). Choosing the other one on a build of the one before
// is a switch (shell/ChannelSwitch.tsx): the dialog says what happens, installs that build now and restarts. A test
// build says so here, with "Switch back to the release".
// The Nexus Mods build (api/distribution.ts) has none of that: one line, "New versions: Nexus Mods", whose link opens
// the Kiln page there, and where test builds are (the download page, which has launchers that update themselves); no
// update command is called.
import { useEffect, useState } from "react";
import * as api from "../api/commands";
import { toKilnError, type UpdateStatus } from "../api/commands";
import { buildLabel } from "../api/channel";
import { DIRECT_DOWNLOAD_PAGE, NEXUS_PAGE } from "../api/distribution";
import { useDistribution } from "../api/hooks";
import { useUpdate, useUpdateStore } from "../api/update";
import { formatLocale, t, useT } from "../i18n";
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

/** When a check was, in local time: the time today, else the day and the time. */
export function checkedTime(iso: string, now = new Date()): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return iso;
  const time = { hour: "numeric", minute: "2-digit" } as const;
  return d.toDateString() === now.toDateString()
    ? d.toLocaleTimeString(formatLocale(), time)
    : d.toLocaleString(formatLocale(), { month: "short", day: "numeric", ...time });
}

/** Why the last check failed: each source's answer (update.rs's words), else the status's error, else that none
 *  answered. */
export function checkFailure(s: UpdateStatus): string {
  if (s.failures.length) return s.failures.map((f) => `${f.source}: ${f.message}`).join("; ");
  return s.error?.message || t("updateSettings.noAnswer");
}

const upToDateWords = (s: UpdateStatus) => t("updateSettings.upToDateLine", { version: buildLabel(s.current, s.beta ?? null) });

/** The line about the launcher's own update check: while it runs, that it does; after it, what it found (Kiln up to
 *  date and its version, the version available, why it failed) and when. `busy`: this window's Check now is under way;
 *  `error`: that call failed before the backend could check (it shows until the next status). */
export function checkLine(s: UpdateStatus, o: { busy?: boolean; error?: string | null; now?: Date } = {}): { text: string; failed: boolean } {
  if (o.busy || s.phase === "checking") return { text: t("updateSettings.checkingNow"), failed: false };
  if (o.error) return { text: t("updateSettings.failedLine", { reason: o.error }), failed: true };
  let what: string;
  if (s.phase === "upToDate") what = upToDateWords(s);
  else if (s.phase === "available" && s.available) what = t("updateSettings.availableLine", { version: buildLabel(s.available.version) });
  else if (s.phase === "failed") what = t("updateSettings.failedLine", { reason: checkFailure(s) });
  else if (s.phase === "skipped") what = updateWords(s);
  else return { text: updateWords(s), failed: false };
  const when = s.checked ? ` · ${t("updateSettings.checkedAt", { time: checkedTime(s.checked, o.now) })}` : "";
  return { text: what + when, failed: s.phase === "failed" };
}

/** checkLine as text; a failed check as an error. */
export function CheckLineText({ s, busy, error }: { s: UpdateStatus; busy?: boolean; error?: string | null }) {
  useT();
  const line = checkLine(s, { busy, error });
  return line.failed ? <span className="k-error-text">{line.text}</span> : <>{line.text}</>;
}

/** The toast after Check now: up to date (and the version), skipped, or failed (and why). An update found has its own
 *  notice (desktop) or row (Deck). */
export function checkToast(s: UpdateStatus) {
  if (s.phase === "upToDate") toast.ok(upToDateWords(s));
  else if (s.phase === "skipped") toast.info(updateWords(s));
  else if (s.phase === "failed") toast.error({ code: "internal", message: checkFailure(s) }, t("updateSettings.failed"));
}

/** Check now: asks once, opens the notice when an update is found, says the rest in a toast (checkToast). The line
 *  (checkLine) shows the answer too; `error` is why the call itself failed, until the next status. */
export function useCheckNow() {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const status = useUpdate();
  useEffect(() => setError(null), [status]);
  const run = async () => {
    setBusy(true);
    setError(null);
    try {
      const s = await api.updateCheck();
      if (s.phase === "available") useUpdateStore.getState().setOpen(true);
      else checkToast(s);
    } catch (e) {
      const k = toKilnError(e);
      setError(k.message);
      toast.error(k, t("updateSettings.failed"));
    } finally {
      setBusy(false);
    }
  };
  return { busy, error, run };
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
      <span role="status" data-testid="update-line"><CheckLineText s={s} busy={check.busy} error={check.error} /></span>{" · "}
      <button type="button" className="k-link" disabled={check.busy || s.phase === "checking"} onClick={check.run}>{t("updateSettings.checkNow")}</button>
    </span>
  );
}
