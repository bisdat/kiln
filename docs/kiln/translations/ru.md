# Russian (ru): notes for reviewers

Kiln's Russian texts (Русский) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/91). The files are
[`launcher/src/i18n/ru.json`](../../../launcher/src/i18n/ru.json) (the Kiln Launcher) and
[`runtime/locales/ru.json`](../../../runtime/locales/ru.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: `launcher/src/i18n/ru.json` (4010 texts), `runtime/locales/ru.json` (805 texts). Both checks pass with 0 untranslated.

## Address form
Polite "вы", lower case. Buttons and menu entries use the infinitive ("Установить", "Удалить"), as Russian software does;
hints and notices use the polite imperative ("Закройте игру", "Выберите мод").

## Plural forms
one, few, many, other everywhere English has a plural. `other` (fractions) uses the genitive singular ("1,5 мода").
Where English `.one` has no `{count}` (the check refuses adding it), the Russian `one` form has no number either; for
21, 31... it then reads as "one" (e.g. `deck.home.sfModSkipped.one` "один мод пропущен", `shell.startProgress.basisSame.one`).
Many count texts with no plural pair in English were rephrased as "Label: {n}" ("Найдено: {count}") to avoid wrong case endings.

## Glossary
| English | Russian |
|---|---|
| mod / mods | мод / моды |
| profile | профиль |
| runtime (Kiln's in-game part, Runtime tab) | рантайм, вкладка «Рантайм» |
| loader | загрузчик |
| engine (another loader's plugin) | движок |
| plugin | плагин |
| native mod | нативный мод |
| report / support report | отчёт / отчёт для поддержки |
| crash report | отчёт о сбое |
| modpack | модпак |
| launcher | лаунчер |
| server / dedicated server | сервер / выделенный сервер |
| world | мир |
| test build / beta / release | тестовая сборка / бета / релиз |
| channel | канал |
| Library | библиотека |
| bundle (runtime bundle) | пакет (пакет рантайма) |
| backup | резервная копия (short: копия) |
| save | сохранение |
| config | конфиг |
| setting | настройка |
| switch on / off | включить / выключить |
| lit / put out (flame toggles) | горит, зажечь / потушен, потушить |
| cook (make mod assets) | запекать, «Запечь заново» |
| roll back | откатить, откат |
| safe idle | безопасный простой |
| step aside (for another loader) | уступить место |
| vanilla | ванилла, ванильный |
| drop-in mods / folders | ручные моды / папки для ручных модов |
| hook | хук, перехватывать |
| load order | порядок загрузки |
| upscaler | апскейлер |
| frame cap | ограничение кадров |
| readout | показатели |
| waypoint | путевая точка |
| grave / tombstone | могила / надгробие |
| protection zone | защитная зона |
| permission node | узел разрешений |
| user group (server password group) | группа пользователей |
| spawn (admin tool) | призыв, призвать |
| respec | сброс навыков |
| skill points | очки навыков |
| perk | перк |
| buff | бафф |
| crafting station | станция крафта |
| the Flame / Flame level | Пламя / уровень Пламени |
| the Shroud | Пелена |
| survivors | выжившие |
| base | база |
| Gem Forge | Кузня самоцветов |
| gem | самоцвет |
| creative mode | режим творчества |
| guided checks | пошаговые проверки |
| Community page | «Сообщество» |
| support post | обращение (в поддержку) |
| tester | тестер |
| Play (tab) / Play (button) | вкладка «Игра» / «Играть» |

## Please check
- **Game terms.** I could not confirm Enshrouded's official Russian terms. Please replace with the game's own words
  where they differ: the Shroud (I used «Пелена»), the Flame and Flame level («Пламя», «уровень Пламени»), survivors
  roster («список выживших»), tombstone («надгробие»), glider («планер»), workbench («верстак»), the game's difficulty
  options in `admin.gameSettings.*` (body heat, startling a tame, curse strength), and item rarities.
- **Cook** = «запекать / запечь» (a kiln bakes; it matches the brand). A reviewer may prefer «собрать».
- **Runtime** = «рантайм» (short for tabs). The alternative «среда выполнения» is too long for the nav.
- **Native load phases** Dllmain, Early, Boot, World are kept in English as phase names (they match native.json values).
- **Steam / Steam Deck menu names** in `deck.help.*` (Свойства, Общие, Параметры запуска, Питание, Переключиться на
  рабочий стол, Добавить стороннюю игру в мою библиотеку, Добавить выбранные программы, Сторонние, Геймпад) are from
  memory of the Russian Steam client; please match the current client.
- **DLSS mode names** (Качество, Баланс, Производительность, Ультрапроизводительность, Ультракачество) follow NVIDIA's
  Russian naming as I know it.
- **Nexus Mods "Endorsements"** = «одобрения».
- `servers.list.namePlaceholder` "Ember Isles" became «Тлеющие острова» (a sample name, free to change).
- Confirm words: `delete` and `uninstall` are both «удалить»; `restore` is «вернуть» (shorter than «восстановить»),
  `switch` is «сменить», `clean` «очистить», `forget` «забыть».
- Gendered past tense for the user (e.g. «Я проверил файлы игры в Steam», «Я вступил», «Не смог проверить») uses the
  masculine form; a neutral rephrase may be nicer.

## Labels that could not stay short
Russian runs longer than English; most labels sit at 130-180% of the English length. The longest ones:
`settings.graphics.labels.dlssUltraPerformanceScale` («Масштаб рендера DLSS, «Ультрапроизводительность»»),
`graphics.mode.ultra_performance` («Ультрапроизводительность»), `deck.help.addNonSteam`,
`deck.help.switchToDesktop`, `admin.zones.action.terraform` («Копать и менять рельеф»),
`admin.zones.action.interact` («Открывать и использовать»), `admin.saves.restoreAtNext`,
`settings.launcher.savesKeep` («Сколько хранить копий сохранений»), `profiles.chain.hookedFirst/hookedOver`,
`saveBackups.backUpNow` / `servers.backups.now` («Копировать сейчас»), `nav.about` («О программе»),
`common.copy` («Копировать»). Shortened on purpose: Up to date → «Актуально», State → «Статус», Count → «Кол-во».
