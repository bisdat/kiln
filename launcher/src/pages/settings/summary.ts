// The live line under each section in Settings' list ("English · fire on · Deck: auto") and the sentence under the open
// section's name.
import { useAppInfo, useActiveProfile, useDistribution, useGame, useGameStatus, useNexusEnabled, useProfiles, useSettings } from "../../api/hooks";
import { totalCount } from "../../api/kinds";
import { useUpdate } from "../../api/update";
import { updateWords } from "../../components/UpdateSettings";
import { choiceOf } from "../../components/upscalerStatus";
import { uiModeOf } from "../../deck/mode";
import { languageName, useLanguage, useT } from "../../i18n";
import { useNexusAccount } from "../../shell/catalogInstall";
import type { SectionId } from "./model";
import { fpsCapOf } from "./searchIndex";

export interface Summaries { line: Record<SectionId, string>; say: Record<SectionId, string> }

const folderName = (p: string) => p.split(/[\\/]/).pop() ?? p;

export function useSummaries(values: unknown): Summaries {
  const t = useT();
  const lang = useLanguage();
  const s = useSettings();
  const info = useAppInfo();
  const { game, games } = useGame();
  const status = useGameStatus(game?.id);
  const profiles = useProfiles();
  const active = useActiveProfile(game?.id);
  const dist = useDistribution();
  const upd = useUpdate();
  const nexus = useNexusEnabled();
  const account = useNexusAccount();

  const language = languageName(lang);
  const fire = !info.data?.noWebgl && s.data?.animatedFire !== false;
  const mode = uiModeOf(s.data);
  const general = [language, fire ? t("settings.summary.fireOn") : t("settings.summary.fireOff"), t(`settings.summary.deck.${mode}`)].join(" · ");

  const loadedId = active.data?.profileId ?? null;
  const loaded = loadedId ? profiles.data?.find((p) => p.id === loadedId) : null;
  const mods = loaded ? [t("settings.summary.loaded", { name: loaded.name }), t("settings.summary.mods", { count: totalCount(loaded.counts) })].join(" · ")
    : active.isSuccess ? t("settings.summary.noProfile") : "";

  const gameName = game ? folderName(game.path) : "";
  const installed = !!status.data?.runtime.installed;
  const kiln = status.data?.runtime.receipt?.runtime.version ?? "";
  const gameLine = !game ? t("settings.summary.noGame") : !status.data ? gameName
    : installed ? `${gameName} · ${t("settings.summary.kiln", { version: kiln })}` : `${gameName} · ${t("settings.summary.noKiln")}`;

  const cap = fpsCapOf(values);
  // only a dedicated server known: graphics are a game client's
  const serverOnly = !game && games.some((g) => g.kind === "server");
  const graphics = serverOnly ? t("settings.summary.noClient") : !game ? t("settings.summary.noGame") : status.data && !installed ? t("settings.summary.noKiln")
    : [t(`settings.summary.upscaler.${choiceOf(values as never)}`), cap ? t("settings.summary.cap", { fps: cap }) : t("settings.summary.noCap")].join(" · ");

  const updates = dist === "nexus" ? t("settings.summary.nexusBuild") : upd ? updateWords(upd) : "";
  const a = account.data;
  const nexusLine = !nexus ? "" : a?.signedIn ? (a.name ?? t("nexusSettings.signedIn")) : a ? t("settings.summary.nexusOut") : "";
  const online = [updates, nexusLine].filter(Boolean).join(" · ");

  return {
    line: { general, mods, game: gameLine, graphics, online, advanced: t("settings.summary.advanced") },
    say: {
      general: t("settings.sections.general.say", { language }),
      mods: loaded ? t("settings.sections.mods.say", { name: loaded.name }) : t("settings.sections.mods.sayNone"),
      game: !game ? t("settings.sections.game.sayNoGame") : installed ? t("settings.sections.game.say", { version: kiln, game: gameName })
        : t("settings.sections.game.sayNoKiln", { game: gameName }),
      graphics: game ? t("settings.sections.graphics.say", { game: gameName }) : serverOnly ? t("settings.sections.graphics.sayServer") : t("settings.sections.game.sayNoGame"),
      online: t("settings.sections.online.say"),
      advanced: t("settings.sections.advanced.say"),
    },
  };
}
