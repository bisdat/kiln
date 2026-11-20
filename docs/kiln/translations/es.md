# Spanish (es): notes for reviewers

Kiln's Spanish texts (Español) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/93). The files are
[`launcher/src/i18n/es.json`](../../../launcher/src/i18n/es.json) (the Kiln Launcher) and
[`runtime/locales/es.json`](../../../runtime/locales/es.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: `launcher/src/i18n/es.json` (4,010 texts) and `runtime/locales/es.json` (805 texts). All translated.
Plural forms written: `one`, `other`.

## Address form and style
- Neutral international Spanish for Spain and Latin America (most players are in Mexico and Central America).
  It uses "tú" and never "usted" or "vosotros". Regionalisms are avoided.
- Buttons and menu entries use the infinitive ("Instalar", "Guardar"). Hints and notices speak to the player with "tú"
  ("Instala Kiln primero en Runtime.").
- Captions that are capitals in English stay capitals ("TODOS LOS MODS", "ESTADO").
- Confirm words: limpiar, eliminar, olvidar, restaurar, cambiar, desinstalar.

## Glossary (English -> Spanish)
| English | Spanish |
|---|---|
| mod / mods | mod / mods (el mod) |
| profile | perfil |
| runtime (the Runtime page) | runtime / Runtime (kept) |
| loader | cargador |
| engine | motor |
| modpack | modpack |
| launcher | launcher (kept) |
| server / dedicated server | servidor / servidor dedicado |
| world | mundo |
| save (noun); Saves page | partida guardada; Partidas |
| backup | copia de seguridad (short label: Copias) |
| test build | versión de prueba |
| release (channel) | Estable / la versión estable |
| channel / beta | canal / beta |
| build (game or Kiln build) | build (kept) |
| update | actualización / actualizar |
| report / support report | informe / informe de soporte |
| log | registro |
| crash (label) | fallo; crash reports = informes de fallos |
| settings | Ajustes |
| Library | Biblioteca |
| Community | Comunidad |
| cook (mod content) | procesar / procesado |
| safe idle | reposo seguro |
| hook | hook (verb: poner un hook, enganchar) |
| proxy | proxy |
| bundle (runtime zip) | paquete |
| player | jugador |
| admin | administrador (badges: admin) |
| God mode | Modo dios |
| kick / ban | expulsar / vetar |
| waypoint | punto de ruta |
| spawn / respawn | generar / reaparecer |
| tombstone / grave | lápida / tumba |
| item / creature | objeto / criatura |
| crafting station | estación de fabricación |
| the Flame / Flame Altar | la Llama / Altar de la Llama |
| the Shroud | la Bruma |
| Embervale | Embervale (kept) |
| survivors | supervivientes |
| base | base |
| buffs / perks / skills | efectos / ventajas / habilidades |
| loot / stack | botín / pila |
| talent tree / respec | árbol de talentos / reasignar |
| Gem Forge / gem slot | Forja de gemas / ranura de gema |
| creative mode | modo creativo |
| guided checks | pruebas guiadas (BIEN / FALLO / OMITIDA) |
| toast | aviso |
| Overlay | Superposición |
| Keys page / key binding | Teclas / asignación de tecla |
| upscaler / upscaling | escalador / escalado |
| frame generation / frame cap | generación de fotogramas / límite de fotogramas |
| readout (graphics) | indicador |
| vanilla | vanilla (kept) |
| drop-in folders | carpetas drop-in |
| issue (GitHub) | incidencia |
| sign in | iniciar sesión |
| endorsements (Nexus) | recomendaciones |

Names stay as they are: Kiln, ShroudForge, EMF, EML, SET, Flame (the loader), Greeneye, Enshwick, XHL-Core,
shroudlink, Shroudtopia, Ember, Steam, Proton, Wine, Discord, Nexus Mods, GitHub, DLSS, FSR, XeSS, file names, keys.
FSR and XeSS are never called DLSS, and the upscaler texts never say "desbloquear".

## Please check
- **Game terms.** I could not read the game's own es-es texts, so these are best guesses: la Bruma (Shroud), Altar de
  la Llama, supervivientes, Embervale (kept), estación de fabricación, and the game settings names in the admin pages
  ("Aguante", "Calor corporal", "Penalización por inanición", "Ventajas", "Bonificación de amenaza"). In the creative menu:
  "Recorrido" (Journey), "Mochila" (backpack), "Casilla", "Planos" (blueprints), "Banco de trabajo". Please match them to
  the game's Spanish menus.
- **"Flame" alone.** It is either the loader's name (kept) or the game's Flame ("la Llama"). Please check the
  places where it appears.
- **Steam menu names.** These guess Steam's Spanish UI: "Propiedades", "Opciones de lanzamiento" (Spain may show
  "Parámetros de lanzamiento"), "Archivos instalados", "Verificar la integridad de los archivos del juego",
  "Añadir un juego ajeno a Steam a mi biblioteca", and Steam Deck "Energía", "Cambiar a Escritorio".
- **cook = "procesar".** "cocinar" (Unreal's word) was the other choice. "procesar" reads clearer to players.
- **runtime and launcher kept in English.** "entorno de ejecución" was too long for labels. Is "el runtime" fine?
- **ban = "vetar".** "banear" is common among gamers and may read more natural.
- **Unload = "Descargar".** This is the same word as download (servers page, "¿Descargar {name}?"). Is it confusing?
- **Talents.** "Unlock ({0})" became "Obtener ({0})" so that it does not say "desbloquear". Respec is "Reasignar".
- **DLSS mode names.** "Rendimiento / Equilibrado / Calidad / Ultrarrendimiento / Ultracalidad". Please compare them
  with NVIDIA's official Spanish names.
- **`catalog.sf.other_side` / `eml_other_side`** (game). These read "solo en {sides}". If `{sides}` is a bare word like
  "client", it may need an article.
- **Counts without plural keys in English.** These use "(s)" forms: "valor(es) cambiado(s)", "cambio(s)", "error(es)",
  "línea(s)".
- **Search examples** in `admin.items.typeTwo` are now Spanish words ("madera, espada, bruma"). Whether they match depends
  on the language the item names are searched in.
- **Placeholder names.** `servers.list.namePlaceholder` "Ember Isles" became "Islas de Ascuas". The map credit
  "Enshrouded Interactive Map" stays in English as a title.
- **Kept in English.** Discord's "Rich Presence", the role "Tester", the channel names #support and #testing, and
  "endpoint", "sandbox", "reflink", "tarball", "Preset" (DLSS).
- **Height on the map.** It is "A" (altura) where English has "H". West is "O", so the compass has "SO" and "NO".

## Labels longer than English
On/Off became Activado/Desactivado, and Lit became Encendido. Backups became "Copias de seguridad" in titles (short nav
label: "Copias"). Live became "En directo", Frame cap "Límite de fotogramas" and Render size "Tamaño de renderizado".
Spanish has no shorter natural words for these. All of them are under about 2.5 times the English length. Where room
was tight I shortened: "Config." (Configs), "Predet." (Default column), "Duración de bonif. de comida", "Releer archivo",
"Paso máx.". The Deck's bulk switches "Shown on ({count})" and "All on ({count})" became actions ("Activar visibles",
"Activar todos"). Please confirm that wording.
