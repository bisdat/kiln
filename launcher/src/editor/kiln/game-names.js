// Names extracted locally with the game art. Missing/older art keeps the vendored editor's names.
let languages = {};

/** Launcher tags -> LanguageId codes used by the game's loca table (including translator-added locales). */
export function gameLanguage(locale) {
  const code = String(locale).replaceAll('_', '-').toLowerCase();
  const defaults = { en: 'us', de: 'de', fr: 'fr', it: 'it', es: 'es', ru: 'ru', pl: 'pl', ja: 'jp', ko: 'kr', pt: 'br', zh: 'cn', th: 'th', tr: 'tr', uk: 'ua', nl: 'nl', cs: 'cz', hu: 'hu' };
  return code.includes('-') ? code : defaults[code] ? `${code}-${defaults[code]}` : code;
}

export function setGameNames(table) {
  languages = table?.format === 1 && table.languages && typeof table.languages === 'object' ? table.languages : {};
}

const text = (v) => typeof v === 'string' && v.trim() ? v : undefined;
export function gameName(kind, id, locale) {
  return text(languages[gameLanguage(locale)]?.[kind]?.[id]) || text(languages['en-us']?.[kind]?.[id]);
}
