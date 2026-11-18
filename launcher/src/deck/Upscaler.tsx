// (dev.29.18) Deck Settings: the upscaler behind the game's FSR 3 option (docs/kiln/graphics.md "Behind the game's
// FSR 3"), for a client with Kiln in it. Rows: "Upscaler" (A: the game's own FSR 3, AMD FSR, Intel XeSS, in turn; the
// game's kiln.json graphics.upscaler), "AMD FSR version" with AMD FSR (auto, FSR 3.1), "Upscaler now" (what ran or
// runs, why not: the desktop card's words, components/upscalerStatus.ts), and one row per vendor component (A:
// download, update, go on, try again; cancel while it downloads; remove when downloaded). A first download of a vendor's
// licence opens the licence screen (the text in full; the right stick scrolls it; "I agree, download", B back); Remove
// asks first. The flow is the desktop's (components/UpscalerSettings.tsx useUpscalerFiles): the same commands, tasks
// and agreements.
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { ConfigDocument, GameInstall, Json } from "../api/commands";
import { qk, useGameStatus } from "../api/hooks";
import { vendorLicense, vk, type VendorComponent } from "../api/vendor";
import { useT } from "../i18n";
import { toast } from "../ui";
import { licenseFacts, removeText, useUpscalerFiles, type UpscalerAction, type UpscalerRowState } from "../components/UpscalerSettings";
import { AMD_MODES, UPSCALER_CHOICES, amdModeOf, choiceOf, upscalerView } from "../components/upscalerStatus";
import { DeckButton, Dialog, Row, Word } from "./ui";

const TONE: Record<UpscalerRowState, "ok" | "warn" | "err" | "info" | "idle" | "ember"> = {
  notDownloaded: "idle", downloading: "ember", downloaded: "ok", updateAvailable: "info", unavailable: "idle", failed: "err",
};
const next = <T,>(list: readonly T[], cur: T) => list[(list.indexOf(cur) + 1) % list.length];

/** The Deck's upscaler rows of a client (nothing for a server, or before Kiln is in the game). */
export function DeckUpscalerRows({ game }: { game: GameInstall }) {
  const status = useGameStatus(game.id);
  if (game.kind !== "client" || !status.data?.runtime.installed) return null;
  return <Rows game={game} />;
}

function Rows({ game }: { game: GameInstall }) {
  const t = useT();
  const qc = useQueryClient();
  const status = useGameStatus(game.id);
  const key = qk.kilnJson(game.id);
  const doc = useQuery({ queryKey: key, queryFn: () => api.configKilnRead(game.id) });
  const set = useMutation({
    mutationFn: (v: { path: string; value: Json }) => api.configKilnSet(game.id, v.path, v.value),
    onSuccess: (d: ConfigDocument) => qc.setQueryData(key, d), onError: (e) => toast.error(e),
  });
  const files = useUpscalerFiles(game.id);
  const values = doc.data?.values;
  const choice = choiceOf(values);
  const amd = amdModeOf(values);
  const view = upscalerView(choice, amd, status.data?.graphicsState, files.status.data?.components, status.data?.graphicsStepAside ?? []);
  // not disabled while a change is saved: a disabled row would lose the focus between two presses of A
  const ready = !!doc.data;
  const change = (path: string, value: Json) => { if (!set.isPending) set.mutate({ path, value }); };
  const act = (c: VendorComponent, a: UpscalerAction | undefined) => {
    if (a === "cancel") files.cancel(c);
    else if (a === "remove") files.remove(c);
    else if (a) files.download(c);
  };
  return (
    <>
      <Row id="set-upscaler" title={t("deck.settings.upscaler")} a={t("deck.shared.change")} disabled={!ready}
        sub={t(`settings.graphics.upscaler.choiceHint.${choice}`)}
        end={<Word tone="ember">{t(`settings.graphics.upscaler.choice.${choice}`)}</Word>}
        onClick={() => change("graphics.upscaler", next(UPSCALER_CHOICES, choice))} />
      {choice === "amd" && (
        <Row id="set-amd-upscaler" title={t("settings.graphics.upscaler.amdLabel")} a={t("deck.shared.change")} disabled={!ready}
          sub={t("settings.graphics.upscaler.amdHint")} end={<Word tone="ember">{t(`settings.graphics.upscaler.amd.${amd}`)}</Word>}
          onClick={() => change("graphics.amdUpscaler", next(AMD_MODES, amd))} />
      )}
      {view.heading && (
        <Row id="set-upscaler-now" title={t("deck.settings.upscalerNow")} aria-label={t("deck.settings.upscalerNow")}
          sub={[...view.lines, ...(view.detail ? [t("settings.graphics.upscaler.detail", { detail: view.detail })] : [])].join(" ")}
          end={<Word tone={view.heading === "now" ? "ok" : "info"}>{t(`settings.graphics.upscaler.heading.${view.heading}`)}</Word>} />
      )}
      {(files.status.data?.components ?? []).map((c) => {
        const row = files.row(c);
        // the action A takes: the row's first (Remove when downloaded)
        const a = row.actions[0];
        const pct = row.state === "downloading" && row.progress != null ? ` ${Math.round(row.progress * 100)}%` : "";
        return (
          <Row key={c.id} id={`set-vendor-${c.id}`} title={`${c.name} ${c.version}`} disabled={!a} dim={!a}
            a={a ? (a === "remove" ? t("common.remove") : a === "cancel" ? t("common.cancel") : t(`settings.upscalers.action.${a}`)) : undefined}
            sub={row.reason ?? (row.state === "notDownloaded" || row.state === "unavailable" ? `${c.enables} ${row.detail}` : row.detail)}
            end={<Word tone={TONE[row.state]}>{t(`settings.upscalers.state.${row.state}`) + pct}</Word>}
            onClick={() => act(c, a)} />
        );
      })}
      {files.asking && <DeckLicense c={files.asking} onAgree={files.agree} onClose={files.cancelAsk} />}
      {files.removing && (
        <Dialog name="vendor-remove" title={t("settings.upscalers.removeTitle", { name: files.removing.name })} onClose={files.cancelRemove}
          buttons={<><DeckButton danger onClick={files.confirmRemove}>{t("common.remove")}</DeckButton><DeckButton onClick={files.cancelRemove}>{t("common.cancel")}</DeckButton></>}>
          <p>{removeText(files.removing)}</p>
        </Dialog>
      )}
    </>
  );
}

/** The licence screen of a vendor's first download on the Deck: what, size, from, to, the licence in full (the right
 *  stick scrolls it), what the host sees; "I agree, download" passes the hash of the text shown. */
function DeckLicense({ c, onAgree, onClose }: { c: VendorComponent; onAgree: (sha256: string) => void; onClose: () => void }) {
  const t = useT();
  const lic = useQuery({ queryKey: vk.license(c.id), queryFn: () => vendorLicense(c.id) });
  const host = c.hosts.join(", ");
  return (
    <Dialog name="vendor-license" title={t("settings.upscalers.dialog.title", { name: c.name })} wide onClose={onClose}
      buttons={<>
        <DeckButton primary disabled={!lic.data} onClick={() => lic.data && onAgree(lic.data.sha256)}>{t("settings.upscalers.dialog.agree")}</DeckButton>
        <DeckButton onClick={onClose}>{t("common.cancel")}</DeckButton>
      </>}>
      {licenseFacts(c).map(([k, v]) => <p key={k} className="deck-note"><b>{k}:</b> {v}</p>)}
      <p className="deck-note">{t("settings.upscalers.dialog.licenseLead", { vendor: c.vendorName, license: c.license.name })}</p>
      <pre className="deck-scroll deck-license" tabIndex={-1} aria-label={c.license.name} data-testid="deck-license">{lic.data?.text ?? (lic.error ? String((lic.error as { message?: string }).message ?? "") : "")}</pre>
      <p className="deck-note" data-testid="deck-license-privacy">{t("settings.upscalers.dialog.privacy", { host })}</p>
    </Dialog>
  );
}
