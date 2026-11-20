# Polish (pl): notes for reviewers

Kiln's Polish texts (Polski) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/96). The files are
[`launcher/src/i18n/pl.json`](../../../launcher/src/i18n/pl.json) (the Kiln Launcher) and
[`runtime/locales/pl.json`](../../../runtime/locales/pl.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: `launcher/src/i18n/pl.json` (4010 texts) and `runtime/locales/pl.json` (805 texts). Both checks pass with 0 texts
left out. Machine translation by an AI agent: please read it as a native speaker would.

## Address form
- Informal "ty", lower case in the middle of a sentence ("twoje mody", "Zamknij grę"), as Polish games and launchers do.
- Buttons are imperatives ("Zainstaluj", "Usuń", "Wczytaj"); tabs and column heads are nouns ("Zapisy", "Kopie zapasowe").
- Status lines use impersonal past forms to avoid gender ("Usunięto", "Zapisano", "Włączono {count} modów").
- Some first and second person past forms are masculine: "Zweryfikowałem pliki gry", "Dołączyłem", "Nie mogłem
  wypróbować", "Tylko to, co znalazłem", "Anulowałeś", "Umieściłeś", "wybrałeś", "Co robiłeś?". Please say if a neutral
  wording reads better. Names of other people use "odpowiedział(a)" / "przeczytał(a)".

## Plurals
- Every plural has one, few, many and other. `other` (fractions in CLDR) uses the `many` wording, so it still reads well
  if some code path falls back to `other` for any number.
- Where English counts without a plural object ("{count} skipped", "{n} found"), Polish uses "Label: {count}" to avoid
  wrong case endings ("Pominięto ({count}): ...", "Znaleziono: {n}").

## Glossary (English -> Polish)
| English | Polish |
|---|---|
| mod / mods | mod / mody (gen. pl. modów) |
| profile | profil |
| runtime (Kiln's files in the game; the Runtime page) | środowisko; page "Środowisko" |
| runtime bundle | pakiet środowiska |
| loader | loader (pl. loadery) |
| engine (SF, EMF... inside Kiln) | silnik |
| engine plugin | wtyczka silnika |
| native mod / plugin | mod natywny / wtyczka |
| report / support report | raport / raport dla wsparcia |
| Kiln support | wsparcie Kiln |
| modpack | modpack |
| launcher | launcher |
| server / dedicated server | serwer / serwer dedykowany |
| world | świat |
| test build / release / beta | wersja testowa / wydanie / beta |
| channel | kanał |
| game build | wersja gry |
| update | aktualizacja / Aktualizuj |
| roll back | Przywróć poprzednią / Wróć do {version} |
| save (game file) / Saves | zapis / Zapisy |
| backup | kopia zapasowa (short: kopia) |
| library | biblioteka |
| data folder / game folder | folder danych / folder gry |
| switch on / off | włącz / wyłącz; short "Wł." / "Wył." |
| lit / put out (flame metaphor, server mods) | zapalony / zgaszony |
| safe idle | tryb bezpieczny |
| vanilla | czysta gra; "Play vanilla" = "Graj bez Kiln" |
| boot (Kiln boot) | rozruch |
| cook (making a mod's models, sounds from the game) | przygotować / przygotowanie |
| hook | hook (pl. hooki), "hookuje" |
| step aside (a loader yields to another) | ustąpić |
| crash / crash report | awaria / raport awarii |
| overlay / toast | nakładka / powiadomienie |
| keys, binding | klawisze, przypisanie |
| upscaler | skalowanie (metoda skalowania); never "odblokuj" |
| frame generation / frame cap / readout | generowanie klatek / limit klatek / odczyt |
| sign in / out | zaloguj się / wyloguj się |
| permissions, node, rule | uprawnienia, węzeł, reguła |
| waypoint | punkt nawigacyjny |
| spawn (admin action) | przywołać / przywoływanie |
| creature / enemy / item | stwór / wróg / przedmiot |
| loot / drop table | łup / tabela łupów |
| crafting / crafting station | wytwarzanie / stanowisko rzemieślnicze |
| creative mode | tryb kreatywny |
| guided checks | testy z przewodnikiem |
| talent tree, respec | drzewko talentów, reset (umiejętności) |
| XP | PD (punkty doświadczenia) |
| Gem Forge | Kuźnia Klejnotów |
| the Flame / Flame level | Płomień / poziom Płomienia |
| the Shroud | Całun |
| Survivors (roster) | ocaleni ("lista ocalonych") |
| tombstone / grave | nagrobek / grób |
| glider | lotnia |
| rarity: common ... legendary | pospolity, niepospolity, rzadki, epicki, legendarny |
| endorsements (Nexus) | polecenia ("Najczęściej polecane") |
| confirm words | wyczyść, usuń, zapomnij, przywróć, przełącz, odinstaluj |

## Please check
1. **Game terms from Enshrouded's Polish version**: the Shroud ("Całun"), the Flame ("Płomień"), Survivors ("ocaleni"),
   crafting station ("stanowisko rzemieślnicze"), glider ("lotnia"), rarity names, and the game's own server settings
   names in `admin.gameSettings` (translated freely: "Wytrzymałość", "Osłabienie z głodu", "Premia zagrożenia"...). Use
   the game's wording if it differs.
2. **DLSS modes** are translated as Polish games usually show them: Jakość, Zrównoważony, Wydajność, Ultrawydajność,
   Ultrajakość (`graphics.mode.*` in game, DLSS labels in launcher settings). If Enshrouded's Polish menu keeps the English
   names, change them back.
3. **"Runtime" = "Środowisko"**: a short Polish word for the page; some Polish modders may prefer "Runtime".
4. **"Cook" = "przygotować"**: neutral; the furnace pun of Kiln ("wypalić") was not used. Check it reads clearly.
5. **Confirm words keep Polish letters** (usuń, wyczyść, przywróć, przełącz): typed exactly. If ASCII words are wanted
   (e.g. "skasuj", "odzyskaj"), change them.
6. **Steam's Polish labels** in Deck help and launch-option hints: Biblioteka, Właściwości, Ogólne, Opcje uruchamiania,
   Zasilanie, Przełącz na pulpit, Dodaj do biblioteki grę spoza Steam, Dodaj wybrane programy, Spoza Steam,
   "Zweryfikuj spójność plików gry". Please match the current Steam client.
7. **Compass letters** on the admin map stay N, NE, E... (Polish "Pn-Wsch" was too wide for the map); "W" there means west.
8. Polish quotes „…” are used where English has “…”; straight quotes where English has straight ones.
9. Kiln terms kept as English loanwords: loader, hook, modpack, launcher, tick, proxy, preset, Rich Presence.

## Labels longer than about 130% of English
Most are common Polish phrases with no shorter natural form: "Uruchom ponownie" (Restart), "Kopie zapasowe" (Backups),
"Konfiguracje" (Configs), "Przywoływanie" (Spawn), "Punkt nawigacyjny" (Waypoint), "Przygotuj ponownie" (Cook again),
"Najczęściej polecane" (Most endorsed), "Aktualizuj" (Update), "Testy z przewodnikiem" (Guided checks), "Tryb kreatywny"
(Creative; the in-game HUD line uses "Kreatywny"), "Usuń przypisanie" (Unbind), "Statystyki ticków" (Tick stats),
"O programie" (About). Worth a look on the Deck and in the in-game Mods page if any of these get cut.
