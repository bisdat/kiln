// (29.18, inbox 92) A runtime change's error as Runtime's and Play's banners say it. For a file Windows would not let
// Kiln write (kiln_manage::blocked: access denied or a sharing violation on a DLL or exe, `detail.blocked`): that an
// antivirus or another program may be blocking it, the file, what to do with a link to the docs' Antivirus section,
// and the raw error under it. Any other error: its message, as before.
import type { ReactNode } from "react";
import * as api from "../api/commands";
import type { KilnError, Platform } from "../api/commands";
import { rich, t as tr, useT } from "../i18n";
import { toast } from "../ui";
import { ANTIVIRUS_DOCS, blockedFile, gamePath } from "./runtimeFiles";

/** `children`: what the banner says after the error (which Kiln the game keeps). */
export function RuntimeFailure({ error, platform, fallback, children }: { error: KilnError | null | undefined; platform: Platform | undefined; fallback: string; children?: ReactNode }) {
  const t = useT();
  const b = blockedFile(error);
  if (!b || !error) {
    return <><span style={{ overflowWrap: "anywhere" }}>{error?.message ?? fallback}</span>{" "}{children}</>;
  }
  return (
    <>
      <div data-testid="runtime-blocked">
        {rich("shell.runtimeBlocked.file", { file: <span className="k-mono">{gamePath(b.path, platform)}</span> })} {t("shell.runtimeBlocked.why")}
      </div>
      <div>
        {t("shell.runtimeBlocked.fix")} {children}{" "}
        <button type="button" className="k-link" onClick={() => void api.appOpenUrl(ANTIVIRUS_DOCS).catch((e) => toast.error(e))}>{t("runtime.antivirusHelp")}</button>
      </div>
      <div className="k-hint" data-testid="runtime-blocked-raw" style={{ overflowWrap: "anywhere" }}>{t("shell.runtimeBlocked.raw", { error: error.message })}</div>
    </>
  );
}

/** The same in one plain line (a toast's title, the Deck's Kiln screen); null for any other error. */
export function blockedLine(error: KilnError | null | undefined, platform: Platform | undefined): string | null {
  const b = blockedFile(error);
  return b ? `${tr("shell.runtimeBlocked.file", { file: gamePath(b.path, platform) })} ${tr("shell.runtimeBlocked.why")}` : null;
}
