# German (de): notes for reviewers

Kiln's German texts (Deutsch) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/90). The files are
[`launcher/src/i18n/de.json`](../../../launcher/src/i18n/de.json) (the Kiln Launcher) and
[`runtime/locales/de.json`](../../../runtime/locales/de.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: launcher/src/i18n/de.json (4,010 texts), runtime/locales/de.json (805 texts). language.name: "Deutsch".
Plural forms written: one, other.

## Address form
Informal "du" (dein, dir, dich), lower case except at a sentence start. Instructions in du-imperative ("Wähle",
"Starte"); buttons in the infinitive ("Installieren", "Speichern"). A sentence starting "Ihr/Ihre" means "its/their",
not the formal "you".

## Glossary (English -> German)
Kiln concepts
- mod -> die Mod (pl. Mods; feminine throughout: "eine Mod", "sie"); mod author -> Mod-Autor; native mod -> native Mod
- modpack -> Modpack; profile -> Profil; runtime -> Runtime (Kiln-Runtime); runtime bundle -> Runtime-Paket
- loader -> Loader; mod engine -> Engine; "stepped aside" -> zurückgetreten; load order -> Ladereihenfolge
- report / support report -> Bericht / Supportbericht; launcher -> Launcher; server -> Server (der);
  dedicated server -> dedizierter Server
- world -> Welt; save -> Spielstand; backup -> Sicherung; restore -> wiederherstellen
- test build -> Testversion; channel -> Kanal; release -> Release / Release-Version; beta -> Beta
- catalog -> Katalog; library -> Bibliothek; logs -> Protokolle; settings -> Einstellungen; config -> Konfiguration
  (tab "Konfigs")
- install / uninstall -> installieren / deinstallieren; enable / disable -> aktivieren / deaktivieren;
  switch on / off -> einschalten / ausschalten; on / off -> an / aus; roll back -> zurückrollen
- cook (asset cooking) -> aufbereiten ("Neu aufbereiten", "Wird aufbereitet")
- safe idle -> sicherer Leerlauf; vanilla -> Vanilla; hook -> Hook ("gehookt"); tick -> Tick
- toast -> Meldung; notice -> Hinweis; crash -> Absturz; key binding -> Tastenbelegung; overlay -> Overlay
- creative mode -> Kreativmodus (menu entry "Kreativ"); admin map -> Admin-Karte; guided checks -> Geführte Tests
- Gem Forge -> Edelsteinschmiede; gem -> Edelstein; talent tree -> Talentbaum; ability -> Fähigkeit;
  cooldown -> Abklingzeit
- upscaler -> Upscaler; frame cap -> Bildratenlimit; render scale -> Renderskala; preset -> Preset
- sign in / out -> anmelden / abmelden; account -> Konto; post -> Beitrag; tester -> Tester; endorsements -> Empfehlungen
- Confirm words: clean = aufräumen, delete = löschen, forget = vergessen, restore = zurückholen (wiederherstellen is
  over the 12-character limit), switch = wechseln, uninstall = entfernen
Game terms (unconfirmed against Enshrouded's German localization)
- the Flame -> die Flamme; Flame Altar -> Flammenaltar; Flame level -> Flammenstufe; the Shroud -> der Nebel
- Embervale -> Embervale (kept); base -> Basis; survivors -> Überlebende; crafting station -> Handwerksstation
- item -> Gegenstand; backpack -> Rucksack; workbench -> Werkbank; health / stamina -> Leben / Ausdauer; XP -> EP;
  level -> Stufe; skill points -> Talentpunkte; respec -> Talente zurücksetzen; grave -> Grab; loot bag -> Beutesack
- rarities: Gewöhnlich / Ungewöhnlich / Selten / Episch / Legendär; fog of war -> Kriegsnebel (to keep it apart from Nebel)
- DLSS modes: Qualität / Ausgewogen / Leistung / Ultra-Leistung / Ultra-Qualität

## Please check
- Every game term above, against the German Enshrouded client (above all Nebel, Flammenaltar, Flammenstufe,
  Handwerksstation, Werkbank, rarity names, pause menu "Beenden").
- admin.gameSettings.*: world-setting names and option values should match the game's German server settings
  ("Hunger bis Verhungern", "Flammenstufe für Nebelauflösung", "Zahmes Tier erschrecken", "Perk-Rückerstattung").
- "aufbereiten" for cook (alternatives: kompilieren, bauen); "Mitlaufen" for following a log; "Rundruf" for broadcast.
- Steam UI names quoted in hints: "Eigenschaften", "Startoptionen", "Installierte Dateien", "Spieldateien auf Fehler
  überprüfen", Deck "Zum Desktop wechseln", "Ein Nicht-Steam-Spiel zu meiner Bibliothek hinzufügen": should match
  Steam's German wording exactly.
- Deck tab "Settings" is "Optionen" and "Add mods" is "Mods holen" (space). Profile column "Lit" is "An"; server
  mods Lit / Put out = Entzündet / Gelöscht.
- Placeholder server name "Ember Isles" became "Glutinseln".
- Sentences with {ago}: written as "geändert {ago}" / "{name} hat {ago} geantwortet"; reads well with format.ago
  ("vor 5 Min."), worth a look in the running launcher.
- catalog.sf.other_side / eml_other_side: {sides} is filled with English words, so the German reads "Sie läuft nur für:
  {sides}." to stay grammatical.
- English oddities kept in shape: servers.head.stopped / stoppedKiln have a space before the punctuation;
  settings.render.gpuRestart's English looks like a slip.
- Command syntax and code words stay English: `/creative on <you>`, pre/post hook phases, dllmain/early/boot/world
  phases, policy values verified-only / any.

## Labels longer than about 130% of English
- Launcher: mods.tabs.order "Ladereihenfolge"; servers.settings.preset "Schwierigkeitsvorgabe"; servers.mods.syncTitle
  "Aus der Bibliothek abgleichen"; servers.head.reconnect "Neu mit Kiln.Admin verbinden"; community.posts.delete
  "Meinen Beitrag löschen"; saves.stageRestore / cancelRestore "Wiederherstellung vormerken / abbrechen";
  admin.gameSettings.flameLevelFogRemoval, factoryProductionSpeedFactor, foodBuffDurationFactor;
  admin.map.unmarkFound "Gefunden aufheben"; deck.settings.upscaler; deck.shared.desktopUi "Desktop-Oberfläche";
  shell: "Nach unten", "Ablegen nicht möglich", "Neu starten", "Überspringen", "Auch Vorabversionen",
  "Rückgängig fehlgeschlagen", "Fertige entfernen"; upscalers.state.unavailable "In dieser Version nicht verfügbar";
  common.retry "Erneut versuchen".
- Game: mods.captions.pause_entries "EINTRÄGE IM PAUSENMENÜ"; mods.engines.attention "Braucht Aufmerksamkeit";
  menu.shroudlink.back "Zurück zu den Mods"; checks.verdict.skip shortened to "ÜBERSPR."; shroudforge.labels.waits
  "WORAUF SIE WARTET".

## Same as English on purpose
Words that are the same in German: Version, Launcher, Commit, Backend, Proxy, Filter, Pause, Details, Modpack, Server,
Mods, Runtime, Engines, Spawn, OK, Premium, Editor, Build, URL.
