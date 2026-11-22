// The language of Kiln's own texts in the game (kiln.json `ui.language`: "auto" follows the game's text language, else a
// tag; the runtime's locales/<tag>.json answer it): the choices both Settings offer. The rows themselves are in
// Settings > General (pages/settings/sections/General.tsx). docs/kiln/translating.md.

/** The game's text languages Kiln's in-game texts may be asked for (Kiln.Ui GameLanguage; the in-game side falls back
 * to English for one it has no file for). */
export const GAME_LANGUAGES = [
  "en", "de", "fr", "es", "es-MX", "it", "pl", "pt-BR", "pt-PT", "ru", "uk", "tr", "nl", "cs", "hu", "ja", "ko", "th",
  "zh-CN", "zh-TW",
] as const;

/** The kiln.json value for "follow the game's language". */
export const GAME_AUTO = "auto";
