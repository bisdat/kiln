# Italian (it): notes for reviewers

Kiln's Italian texts (Italiano) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/97). The files are
[`launcher/src/i18n/it.json`](../../../launcher/src/i18n/it.json) (the Kiln Launcher) and
[`runtime/locales/it.json`](../../../runtime/locales/it.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: `launcher/src/i18n/it.json` (4,010 texts) and `runtime/locales/it.json` (805 texts).
`language.name`: "Italiano". Plural forms: `one`, `other`.

## Address form
Informal "tu" everywhere (imperatives: "Chiudi il gioco", "Accedi", "Scegli"), as Italian games and launchers do.
Buttons use the imperative ("Salva", "Aggiorna"); status lines use past participles ("Salvato", "Rimossa").

## Glossary (English -> Italian)
| English | Italian | Note |
|---|---|---|
| mod | mod (feminine: la mod, le mod) | common Italian gaming usage; check gender |
| profile | profilo | |
| runtime | runtime (m.) | |
| loader | loader (m.) | |
| engine (other loaders) | motore | |
| plugin | plugin | |
| launcher | launcher (m.) | |
| Library | Libreria | |
| modpack | modpack (m.) | |
| server / client | server / client | |
| world | mondo | |
| save / save file | salvataggio / file di salvataggio | |
| backup | backup | "Backup ora", "fare il backup" |
| test build | build di prova (f.) | |
| release | release (f.) | |
| beta | beta | |
| channel | canale | |
| support report (zip) | report di supporto | |
| report (an engine's / crash report) | rapporto | kept distinct from the support "report" |
| switch on / off (a mod) | accendere / spegnere; accesa / spenta | matches the flame metaphor ("Lit" = Accesa, "Put out" = Spenta) |
| enable / disable (a setting) | attivare / disattivare; attivo / spento | |
| install / uninstall | installare / disinstallare | |
| roll back | tornare indietro / tornare a {version} | |
| cook (build mod assets) | compilare; "Cook again" = Ricompila | reviewer: maybe a better word |
| safe idle | modalità sicura | |
| step aside | farsi da parte / lasciare il posto | |
| drop-in folder | cartella drop-in | |
| hook | hook / agganciare | |
| load order | ordine di caricamento | |
| config | config (f.) | |
| frame cap | limite fotogrammi | |
| readout (graphics) | indicatore | |
| upscaler | upscaler | |
| render scale | scala di rendering | |
| DLSS modes | Prestazioni, Bilanciato, Qualità, Ultra prestazioni, Ultra qualità | |
| frame generation | generazione di fotogrammi | |
| the Flame | la Fiamma | |
| Flame level | livello della Fiamma | |
| Flame Altar | Altare della Fiamma | (not used in these files, but chosen) |
| the Shroud | la Coltre | UNSURE: please use the game's official Italian term |
| Survivors | Sopravvissuti | |
| crafting station | stazione (di creazione) | |
| workbench | banco da lavoro | |
| tombstone | lapide | |
| grave | tomba | |
| glider | deltaplano | |
| backpack | zaino | |
| stamina | vigore | check against the game |
| rarity: Common ... Legendary | Comune, Non comune, Raro, Epico, Leggendario | |
| NPC | PNG | |
| waypoint | waypoint | |
| respec | redistribuire (punti) | |
| endorsements (Nexus) | apprezzamenti | |
| Rich Presence | Rich Presence | |

## Please check (native speaker)
- **Shroud = "Coltre"** and **stamina = "Vigore"**: I am not sure these match Enshrouded's official Italian localization. Search the files for "Coltre" / "Vigore" and replace if the game says otherwise.
- **Mod gender**: I used feminine (la mod / le mod, "accesa", "rimossa"). If you prefer masculine, many adjectives and participles need changing.
- **Server settings names** (`admin.gameSettings.*`): translated freely; the game's own Italian server-settings labels may differ (e.g. "Da affamato a denutrito", "Spaventare un animale domato", "Bonus minaccia").
- **"Cook" = compilare**: Kiln "cooks" mod models, animations and sounds for the game. "Compilare" is technical; "preparare" or keeping "cook" are options.
- **"Lit / Out" flame wording** (servers Mods page, mod lists): rendered as Accesa / Spenta, so the flame image is weaker than in English.
- `servers.list.namePlaceholder` "Ember Isles" became "Isole di Brace" (a sample server name).
- `checks.verdict` PASS / FAIL as "OK" / "KO" (short, common in Italian test talk).
- `deck.help.power` "Power" (Steam Deck menu entry) = "Spegni"; `addNonSteam`, `addSelected`, "Launch Options" = "Opzioni di avvio": these should match Steam's Italian UI; I wrote them from memory.
- English `servers.head.stoppedKiln` / `stopped` look cut ("Start runs it ."): translated as written, without the stray space.
- Two texts were reworded only to pass the link check (the checker reads "file:" as a URL scheme): `settings.upscalers.gameRunning`, `community.send.check.zipHint`, and in game `catalog.file.unreadable`, `shroudforge.files_unreadable`. Avoid writing "file:" in future edits.

## Labels longer than English (could not keep short)
Italian runs longer; most are under ~130%, these are clearly over and worth a look in the UI:
- `deck.kiln.holdUninstall` "Tieni premuto per disinstallare", `community.deck.holdDelete` "Tieni premuto per eliminare il post"
- `admin.players.respecTree` "Redistribuisci l'albero delle abilità", `admin.players.respec` "Redistribuisci"
- `admin.zones.action.loot` "Saccheggiare e raccogliere", `admin.gameSettings.tamingStartleRepercussion`
- `mods.order.onClient` / `onServer` "Ordine di caricamento su un client/server", `profiles.chain.orderNotChanged`
- `shell.serverNav.combat` / `admin.pages.combat` "Combattimento"
- `settings.graphics.labels.dlssScale` "Dimensione di rendering DLSS"
- `shell.drop.dropsUnavailable` / `ui.dropZone.dropsUnavailable` "Trascinamento file non disponibile"
- In game: `mods.details.load_order` "ORDINE DI CARICAMENTO", `checks.key.pass` "Segna come superato il controllo in corso", `creative.category.instruments` "Strumenti musicali"
- Deck tab `deck.app.tabs.settings` shortened to "Impostaz." and `addMods` to "Aggiungi" to fit.
