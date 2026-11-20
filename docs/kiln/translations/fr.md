# French (fr): notes for reviewers

Kiln's French texts (Français) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/92). The files are
[`launcher/src/i18n/fr.json`](../../../launcher/src/i18n/fr.json) (the Kiln Launcher) and
[`runtime/locales/fr.json`](../../../runtime/locales/fr.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files:
- launcher/src/i18n/fr.json (Kiln Launcher)
- runtime/locales/fr.json (Kiln's texts in the game)

Written for France, readable in Canada (no regionalisms: "courriel" vs "e-mail" never comes up; "fin de semaine" etc. avoided).

## Choices
- Address form: "vous" everywhere. Sentences use the imperative with vous ("Fermez le jeu"); buttons and menu entries
  use the infinitive ("Installer", "Annuler", "Supprimer"), as French software usually does.
- Sentence case for titles and labels (only the first word capitalised). Captions in CAPITALS stay in capitals.
- Typography: a normal space (U+0020) before : ; ! ? (the French rule). The checks refuse non-breaking spaces, so a
  line can in theory break before the sign. English quotes "..." became « ... » with normal spaces inside.
- Plurals: "one" (covers 0 and 1) and "other", everywhere English has a pair. Sentences without a plural pair but with
  a number use "(s)" where agreement needed it ("{count} changement(s)"), as English did there.
- Confirm words: nettoyer, supprimer, oublier, restaurer, basculer, désinstaller (é is a single key on AZERTY and on
  Canadian keyboards).
- Units: Mo, Ko, Kio (French for MB, KB, KiB); "fps" kept (common in French games; "ips" is the formal one).
- Flame switch metaphor: "lit / put out" = "allumé / éteint"; plain on/off = "activé / désactivé".

## Glossary
See GLOSSARY.md in this folder for the full list given to every part. The main terms:

| English | French |
|---|---|
| mod / mods | mod / mods (masc.) |
| native mod | mod natif |
| profile | profil |
| runtime (and the Runtime page) | runtime (kept: page "Runtime") |
| loader | chargeur |
| engine (mod engine) | moteur |
| engine plugin | plugin de moteur |
| stepped aside | s'est effacé (au profit de...) |
| self-hosted | autonome |
| report / support report | rapport / rapport d'assistance |
| modpack | pack de mods |
| launcher | lanceur |
| server / dedicated server | serveur / serveur dédié |
| world | monde |
| save | sauvegarde |
| backup | copie de secours |
| test build | version de test |
| release (published version) | version publiée |
| beta / channel | bêta / canal |
| Library / Catalog | Bibliothèque / Catalogue |
| game folder / data folder | dossier du jeu / dossier de données |
| bundle (runtime bundle) | paquet (paquet du runtime) |
| parts (of a bundle) | composants |
| hook | hook (verb "hooker") |
| overlay | overlay |
| toast | notification |
| key binding | raccourci / liaison (dev window) |
| Keys page | Touches |
| vanilla (no Kiln) | vanilla |
| Without mods | Sans mods |
| safe idle | veille de sécurité |
| sign in | se connecter / connexion |
| upscaling / upscaler | mise à l'échelle / outil de mise à l'échelle |
| frame generation | génération d'images |
| frame cap | limite d'images |
| readout | indicateur |
| preset | préréglage |
| cook (make game assets) | préparer |
| drop-in folders | dossiers à déposer |
| guided checks | vérifications guidées |
| creative mode | mode créatif |
| talent tree | arbre de talents |
| Gem Forge | Forge de gemmes |
| Item details | Détails de l'objet |
| the Flame / Flame Altar | la Flamme / Autel de la Flamme |
| the Shroud | le Voile |
| backpack | sac à dos |
| workbench | établi |
| blueprints | plans |
| fog of war | brouillard de guerre |
| rarity: common ... legendary | commun, peu commun, rare, épique, légendaire |
| endorsements (Nexus) | recommandations |
| waypoint / map marker / route | balise / repère / itinéraire |
| grave | tombe |
| respec | réinitialiser (les compétences) |
| spawn / spawned | faire apparaître / apparu(e)s |
| drop tables / loot | tables de butin / butin |
| NPC | PNJ |
| perk | atout |
| Kiln support (Discord) | l'assistance Kiln |
| channel (Discord) | salon |
| Rich Presence | Rich Presence (Discord's own name) |

## Please check
- Enshrouded's official French terms: "the Shroud" = "le Voile"? "Flame Altar" = "Autel de la Flamme"? perks, glider,
  crafting station, the game's world settings names (admin game settings). I did not have the official localization.
- "Runtime" kept as an English word (page name and concept). Alternatives: "Moteur Kiln", "Composants". "Hook" kept too.
- "vanilla" kept as gamers say it; "Sans mods" is the separate "Without mods" option (Kiln runs, no mods).
- Steam's French UI: "Propriétés", "Options de lancement", "Vérifier l'intégrité des fichiers du jeu",
  "Fichiers installés": checked from memory.
- saveBackups.local: the word Steam Cloud shows for the local copy in its conflict dialog ("locale" used).
- Nexus Mods "Endorsements": "Recommandations" used; Nexus' own French wording may differ.
- DLSS mode names follow NVIDIA's French ("Performance", "Équilibré", "Qualité", "Ultra Performance").
- In game: "Quitter" in gamemenu.summary.* should match the game's French pause-menu entry.
- servers.list.namePlaceholder: "Ember Isles" became "Îles de braise" (an example name only).

## More to check (from the parts)
- admin.gameSettings.*: the world-setting labels (Stamina = Endurance, Body heat = Chaleur corporelle, Starving debuff =
  Malus de famine, Curse strength = Force des malédictions, tombstone options, glider = planeur...) are our wording,
  not the game's. Best replaced with the game's own French texts where they exist.
- Steam client names in the Deck help (deck.help.*): Bibliothèque, Propriétés, Options de lancement, Passer au bureau,
  "Ajouter un jeu non-Steam à ma bibliothèque", "Ajouter les programmes sélectionnés", mode Jeu / mode Bureau.
- community.deck.qr.step3: "then press Approve" became "puis approuvez" (no button name: that web page may be English).
- settings.render.noGpuRestart / gpuRestart: read as "the page had not started with / without the GPU".
- admin.spawn.removeNone ("Remove  spawned", the no-count fallback) -> "Retirer les apparues"; admin.map.graveWaypointNone
  ("Grave grave") -> "Tombe".
- "{pct} %" with a space before % (French rule) in shell.appShell.pct and deck.app.startingPct.

## Labels longer than about 130% of English
French is longer on most very short English labels. The notable ones:
- Respec -> Réinitialiser; Owners -> Propriétaires; Skill points -> Points de compétence; Heal to full -> Soigner entièrement
- Overview -> Vue d'ensemble; Backups -> Copies de secours; Configs -> Configurations; Modpack -> Pack de mods
- On / Off -> Activé / Désactivé; Up -> Remonter; Places -> Emplacements; Hold -> Maintenir; Live -> En direct
- Fog removal flame level -> Niveau de Flamme pour dissiper le brouillard
- Save report instead -> Enregistrer le rapport à la place; Sign in with Discord to send -> Se connecter avec Discord pour envoyer
- Check for mod updates once a day -> Rechercher les mises à jour des mods une fois par jour
- In game: menu.title Mod Manager -> Gestionnaire de mods; mods.captions.pause_entries -> ENTRÉES DU MENU PAUSE;
  checks.key.fail -> "Marquer en échec la vérification en cours (puis écrire une ligne)"

## Left the same as English on purpose
Names and words that are the same in French: Mods, Overlay, Version, Console, Action, Admin, Combat, Rare, Boss,
Instruments, Plugin, Auto, Rich Presence, OptiScaler, DLSSTweaks, units and coordinate formats.

Detailed notes per part: notes-admin.md, notes-community-settings.md, notes-shell-deck.md (same folder).
