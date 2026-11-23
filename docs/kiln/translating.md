# Translating Kiln

Kiln has two sets of texts a player reads: the **Kiln Launcher** (the desktop app) and **Kiln's own texts in the game**
(its menus, windows and messages). Both use one file per language in the same format, the format of a mod's
`locales/*.json` ([modding/lifecycle.md](modding/lifecycle.md#mod-texts-locales)). English is the source; a text a
translation does not have yet shows in English, so a translation can be partial and grow.

## Languages

| Language | Tag | State |
|---|---|---|
| Simplified Chinese (简体中文) | `zh-CN` | by MagicFox (XHL), since 0.2.0-dev.29.18; updated for 0.2.0-dev.30.5 |
| German (Deutsch) | `de` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/de.md), [review issue](https://github.com/frostdev-ops/kiln/issues/90) |
| Russian (Русский) | `ru` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/ru.md), [review issue](https://github.com/frostdev-ops/kiln/issues/91) |
| French (Français) | `fr` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/fr.md), [review issue](https://github.com/frostdev-ops/kiln/issues/92) |
| Spanish (Español) | `es` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/es.md), [review issue](https://github.com/frostdev-ops/kiln/issues/93) |
| Brazilian Portuguese (Português (Brasil)) | `pt-BR` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/pt-BR.md), [review issue](https://github.com/frostdev-ops/kiln/issues/94) |
| Japanese (日本語) | `ja` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/ja.md), [review issue](https://github.com/frostdev-ops/kiln/issues/95) |
| Polish (Polski) | `pl` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/pl.md), [review issue](https://github.com/frostdev-ops/kiln/issues/96) |
| Italian (Italiano) | `it` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/it.md), [review issue](https://github.com/frostdev-ops/kiln/issues/97) |
| Korean (한국어) | `ko` | machine translation, waiting for native speakers (0.2.0-dev.29.19): [notes and glossary](translations/ko.md), [review issue](https://github.com/frostdev-ops/kiln/issues/98) |

A machine translation is complete but not yet read by a native speaker: if your language is one of them, the review
issue says what to look at first, and a comment there or a pull request is all it takes.

## Where the files live

| What | English (the source) | Your language |
|---|---|---|
| Launcher | `launcher/src/i18n/en.json` | `launcher/src/i18n/<language>.json`, e.g. `zh-CN.json` |
| In game ([translating-game.md](translating-game.md)) | `runtime/locales/en.json` | `runtime/locales/<language>.json`; in a game folder `<game>/kiln/locales/<language>.json` |

Name the file by its language tag: `zh-CN.json` (Simplified Chinese), `zh-TW.json`, `de.json`, `pt-BR.json`, `es-419.json`.
A main language (`de.json`) serves all its regions; a regional file (`pt-BR.json`) wins over it for that region.

The save editor inside the launcher (Saves, Edit; the Deck's Editor tab) is covered: its texts are the `editor` keys of the
same files. Item and skill names in it are the game's own and are not translated here.

## The format

One JSON object. Values are text in quotes. A nested object makes dotted keys, so these two say the same:

```json
{ "settings": { "title": "Settings" } }
```
```json
{ "settings.title": "Settings" }
```

Translate the values, never the keys. Keep the nesting as `en.json` has it (copy `en.json`, then replace the English).
The file is UTF-8. Comments (`//`) and a trailing comma are accepted, but plain JSON is safest.

`language.name` is your language's name in your language (`"简体中文"`): the language menus show it.

## Placeholders

The program fills these in. Keep each one exactly as written, move it wherever your grammar needs it:

| In `en.json` | Means | Example |
|---|---|---|
| `{0}`, `{1}` | the first, second value | `"{0} of {1}"` → `"{1} 中的 {0}"` |
| `{name}`, `{count}` | a named value | `"Removed {name}"` → `"已移除 {name}"` |
| `{count:N0}` | a number with a format (`N0`: digit groups, no decimals) | `"{count:N0} icons"` → `"{count:N0} 个图标"` |
| `{{` and `}}` | a literal `{` and `}` | `"{{ ... }}"` shows `{ ... }` |

Don't translate what is inside the braces, don't add placeholders English does not have, don't drop one (except
`{count}` in a plural form, below). Product and file names in a text (Kiln, Enshrouded, Nexus Mods, ShroudForge,
`kiln.json`, `KILN_LAUNCHER_NO_WEBGL`) stay as they are.

## Plurals

Where English changes with a number it has a pair of keys:

```json
"filesRead": { "one": "{count} translation file read", "other": "{count} translation files read" }
```

Fill the forms your language uses (the CLDR plural categories: `zero`, `one`, `two`, `few`, `many`, `other`).
Chinese, Japanese and Korean have one form: write only `other`.

Where a language's `one` covers more than 1 (Russian's is also 21, 31, ...; French and Brazilian Portuguese's also 0),
its `.one` may carry the `{count}` English's `.one` leaves out ("one mod left out" becomes `"{count} мод пропущен"`).

```json
"filesRead": { "other": "已读取 {count} 个翻译文件" }
```

## Keep labels short

Buttons, tabs, menu entries and column heads have little room. For Chinese, Japanese and Korean aim for about **half
the English character count** on those (`"Settings"` → `"设置"`, `"Check for updates"` → `"检查更新"`). Sentences in
hints and notices can be as long as they need.

`common.confirmWord.*` are words a player types to confirm a risky action ("uninstall", "delete"): make each one a
single short word that is easy to type with your language's keyboard or input method.

## Test your translation

**Launcher** (no build needed):

1. Settings, Language shows the folder under "Your translation files": `locales` in the launcher's data folder
   (by default `%LOCALAPPDATA%\Kiln\locales` on Windows, `~/.local/share/kiln/locales` on Linux). Make it if it is not
   there yet.
2. Put your file there: `locales/zh-CN.json`.
3. In Settings, Language, press the reload button next to "Your translation files" (or restart the launcher), then
   choose your language under Launcher. It applies at once.

A file there wins over the launcher's built-in file of the same language, so you can test changes to a translation
the launcher already ships. A broken file is named under the reload button with what is wrong.

**In game:** put the file in `<game>/kiln/locales/` and choose the language in game (Mods, Kiln settings, Language) or
in the launcher's Settings, Language, "Kiln in game" (both set kiln.json `ui.language`; "Same as the game" follows the
game's own language). [translating-game.md](translating-game.md) has the details, the CJK fonts and a preview without
the game.

**Check the file** before you send it (with the repository checked out, from `launcher/`):

```
bun scripts/i18n-check.ts path/to/zh-CN.json
```

It lists keys `en.json` does not have (a typo, or a text the launcher no longer shows), values that are not text,
placeholders and number formats that differ from English (`{count}` where English has `{count:N0}`), and what a
translation never needs where English has none at that key: an HTML tag or entity, a Markdown link, a link or URL, an
invisible or control character (a line break, a tab, a zero-width or right-to-left mark); also an empty text (leave the
key out instead: English shows), a text over four times as long as English's, and a confirm word that is not one short
word. It counts the texts not translated yet. The launcher's unit tests run the same check over every file in
`launcher/src/i18n/`; for the game, `KilnTextKeysTests` checks `runtime/locales/` the same way (and refuses `##`, which
starts an ImGui id, and `%`).

## Send it

- **Preferred:** a pull request on GitHub adding `launcher/src/i18n/<language>.json` (and, for the game,
  `runtime/locales/<language>.json`). Updates later are pull requests changing the same file.
- **Also fine:** send the file(s) on the Kiln Discord or attach them to a GitHub issue; we add them for you.

When English gains texts, they show in English in your language until you add them; the check script lists what is
missing.

## Adding a translation to Kiln

What happens to a file that arrives (the first one, Simplified Chinese by XiaoHuLi666 (XHL), in 0.2.0-dev.29.18):

1. **It is data from outside.** The archive and its sha256 are kept with the wave's notes, it is unpacked into an empty
   folder of its own, and the files go in byte for byte (`launcher/src/i18n/<tag>.json`, `runtime/locales/<tag>.json`),
   so their hashes still match the archive.
2. **The checks**, before the commit: `bun scripts/i18n-check.ts` (from `launcher/`, every file beside `en.json`) and
   Kiln.Ui.Tests' `KilnTextKeysTests` (keys, placeholders, number formats and the data checks above). Nothing they flag
   goes in: the translator fixes it, or the key is left out and English shows. Where a text appears matters: the
   launcher shows texts as plain text (React; the admin map escapes them for its tooltips), the game draws them with
   ImGui and its own menus.
3. **Nothing to wire.** The launcher takes every file in `src/i18n/` (`locales.ts`): Settings, Language and the Deck's
   Settings, Language list it by its `language.name`; plurals follow the language's CLDR rules; dates and numbers follow
   it (`formatLocale()`); a key it lacks shows English. The game lists every file in `kiln/locales` (packaging copies
   `runtime/locales/*.json`).
4. **Look at it.** `KILN_LANG_SHOTS=<dir> bun x playwright test tests/language.spec.ts` (from `launcher/`; it covers
   the shipped zh-CN on the desktop and the Deck: copy its tests for another language) saves screenshots; for the game,
   `tools/ui-preview/preview.sh --language <tag> --locales runtime/locales`.
5. **Fonts.** The launcher's own fonts are Latin: Chinese, Japanese and Korean come from the system's fonts (the
   page's `lang` is the file's tag, so the system picks that language's glyph shapes). In game, a CJK language uses the
   game's own font of that language group ([translating-game.md](translating-game.md#chinese-japanese-and-korean)), a
   Cyrillic one the game's Cyrillic font of its default group
   ([translating-game.md](translating-game.md#cyrillic-and-the-other-scripts-of-the-games-default-font-group)); until it
   is in the overlay, Kiln's texts there are English and a language's name in the Language entry is its tag.

## Credit

Translators are credited in [CREDITS.md](../../CREDITS.md) under Translations, with the language and the name you want
shown, and in the release notes of the release that ships the translation.
