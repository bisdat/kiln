// A controller-only local export: confirm a native Downloads/home destination inside the launcher. Native folder
// dialogs have not been qualified under gamescope, so this flow never opens one, signs in, or uploads a pack.
import { useQueryClient } from "@tanstack/react-query";
import { useEffect, useRef, useState } from "react";
import * as api from "../api/commands";
import type { BuiltPack, KilnError, ProfileId } from "../api/commands";
import { useT } from "../i18n";
import { DeckButton, Dialog, Word } from "../deck/ui";
import { changedFilesLabel, libraryChangedOf, takeOver } from "../shell/libraryChanged";
import { toast } from "../ui";
import { buildPackFile, defaultExportFolder } from "./localExport";

export function DeckExportDialog({ profileId, profileName, onClose }: { profileId: ProfileId; profileName: string; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const [dir, setDir] = useState<string | null>(null);
  const [error, setError] = useState<KilnError | null>(null);
  const [result, setResult] = useState<BuiltPack | null>(null);
  const [busy, setBusy] = useState(false);
  // State alone cannot guard two A/click events before React commits the disabled button.
  const inFlight = useRef(false);
  useEffect(() => {
    let live = true;
    defaultExportFolder().then((p) => { if (live) setDir(p); }, (e) => { if (live) setError(api.toKilnError(e)); });
    return () => { live = false; };
  }, []);
  const changed = libraryChangedOf(error);
  const save = async (recover = false) => {
    if (!dir || inFlight.current || result) return;
    inFlight.current = true;
    setBusy(true);
    setError(null);
    try {
      if (recover && changed && !await takeOver(qc, changed.entry, changed.name, true)) {
        setError({ code: "hash_mismatch", message: t("shell.libraryChanged.failed", { name: changed.name }), detail: error?.detail });
        return;
      }
      setResult(await buildPackFile(profileId, profileName, dir));
    } catch (e) { setError(api.toKilnError(e)); }
    finally { inFlight.current = false; setBusy(false); }
  };
  const close = () => {
    if (inFlight.current) return;
    if (!result && !error) toast.info(t("crucible.deck.exportCanceled"));
    onClose();
  };
  // A new phase chooses focus afresh: destination loading -> Save; Library error -> recovery; done -> OK.
  const phase = busy ? "busy" : result ? "done" : changed ? "changed" : dir ? "ready" : "destination";
  return (
    <Dialog key={phase} name="pack-export" title={result ? t("crucible.save.done", { name: profileName }) : t("crucible.deck.exportTitle", { name: profileName })}
      onClose={close} locked={busy} backLabel={result ? t("common.close") : t("common.cancel")} buttons={result
        ? <DeckButton primary autoFocus id="pack-export-done" onClick={close}>{t("common.ok")}</DeckButton>
        : <>
          <DeckButton id="pack-export-cancel" disabled={busy} onClick={close}>{t("common.cancel")}</DeckButton>
          {changed
            ? <DeckButton primary autoFocus id="pack-export-recover" disabled={busy} onClick={() => void save(true)}>{changedFilesLabel()}</DeckButton>
            : <DeckButton primary autoFocus id="pack-export-confirm" disabled={busy || !dir} onClick={() => void save()}>{error && dir ? t("common.retry") : t("crucible.save.button")}</DeckButton>}
        </>}>
      <div className="deck-scroll deck-list">
        {result ? <>
          <p className="deck-line">{t("crucible.deck.exportLocation")}</p>
          <p className="deck-note k-mono" style={{ overflowWrap: "anywhere" }} data-testid="deck-export-file">{result.file}</p>
          {result.notes.map((note, i) => <p className="deck-note" key={i}>{note}</p>)}
        </> : <>
          <p className="deck-line">{t("crucible.deck.exportLocal")}</p>
          <p className="deck-note">{t("crucible.share.confirmNever")}</p>
          <p className="deck-line">{t("crucible.deck.exportDestination")}</p>
          {dir ? <p className="deck-note k-mono" style={{ overflowWrap: "anywhere" }} data-testid="deck-export-destination">{dir}</p>
            : !error && <p className="deck-note" role="status">{t("crucible.deck.exportFinding")}</p>}
          <p className="deck-note">{t("crucible.deck.exportReplace")}</p>
        </>}
        {busy && <p className="deck-line" role="status">{t("crucible.deck.exportSaving")}</p>}
        {error && <p className="deck-line" role="alert" data-testid="deck-export-error"><Word tone="err">{error.code === "canceled" ? t("crucible.deck.exportCanceled") : error.message}</Word></p>}
        {changed && <p className="deck-note">{t("shell.libraryChanged.detail")}</p>}
      </div>
    </Dialog>
  );
}
