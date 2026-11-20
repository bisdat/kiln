# Brazilian Portuguese (pt-BR): notes for reviewers

Kiln's Brazilian Portuguese texts (Português (Brasil)) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/94). The files are
[`launcher/src/i18n/pt-BR.json`](../../../launcher/src/i18n/pt-BR.json) (the Kiln Launcher) and
[`runtime/locales/pt-BR.json`](../../../runtime/locales/pt-BR.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: `launcher/src/i18n/pt-BR.json`, `runtime/locales/pt-BR.json` (worktree dev29/translations).

## Address form and style
- "você", informal and direct, as Brazilian launchers and games usually write.
- Buttons, menu entries and tabs use the infinitive ("Instalar", "Salvar", "Voltar versão").
- Instructions in sentences use the imperative ("Instale o Kiln", "Feche o jogo primeiro").
- Plurals: `one` and `other`. CLDR `one` in pt-BR covers 0 and 1, so every `one` form keeps `{count}`.
  A "0" may therefore read as singular ("0 mod ativado"). That is correct Brazilian usage.

## Glossary (English -> pt-BR)
| English | pt-BR |
|---|---|
| mod / modpack | mod / modpack |
| profile | perfil |
| runtime | runtime |
| loader | loader |
| launcher | launcher ("Kiln Launcher" unchanged) |
| Library | Biblioteca |
| Catalog | Catálogo |
| server / client | servidor / cliente |
| world | mundo |
| test build | versão de teste |
| release (stable, vs beta) | versão estável |
| channel / beta channel | canal / canal beta |
| report / support report | relatório / relatório de suporte |
| save (game save) | save (o save, os saves) |
| backup / back up | backup / fazer backup |
| switch on / off | ativar / desativar; states "Ativado" / "Desativado" |
| switch (the toggle control) | chave |
| Light (a mod, Kiln's flame metaphor) | ativar (flame tooltips: acender / apagar) |
| delete / remove | excluir / remover |
| refresh / update | recarregar / atualizar |
| reset | redefinir |
| roll back | voltar versão |
| game folder / data folder | pasta do jogo / pasta de dados |
| game data | dados do jogo |
| engine / engine plugin | engine / plugin de engine |
| native mod | mod nativo |
| load order / phase | ordem de carregamento / fase |
| step aside (another loader wins) | dar passagem ("deu passagem") |
| cook (make a mod's assets) | processar (processado, "Processar de novo") |
| safe idle | modo seguro ocioso |
| vanilla | original ("Iniciando o jogo original") |
| crash | travamento |
| key binding | atalho |
| overlay | overlay |
| upscaler | upscaler |
| frame cap / frame generation | limite de quadros / geração de quadros |
| graphics readout | painel de desempenho |
| render scale / preset | escala de renderização / predefinição |
| admin page | página de admin |
| player / survivors | jogador / Sobreviventes |
| spawn (page, action) | gerar; enemy spawn: surgimento |
| waypoint | ponto de referência |
| stamina | vigor |
| tombstone / grave | lápide / túmulo |
| loot | saque |
| respec | redistribuir |
| Flame / Flame Altar | Chama / Altar da Chama |
| Shroud | Névoa |
| Embervale | Embervale |
| crafting station | estação de produção |
| sign in / sign out | entrar / sair; state: conectado / desconectado |
| confirm words | limpar, excluir, esquecer, restaurar, trocar, desinstalar |

## Please check (native speaker)
- Official Enshrouded pt-BR game terms were not verified: Chama, Altar da Chama, Névoa (Shroud), Sobreviventes,
  estação de produção, vigor, lápide, túmulo, Projetos (blueprints), Encaixe de gema, bancada, Jornada, mochila,
  planar / planador (glider). Please swap in the game's own words where they differ.
- "Runtime" (nav tab and the Kiln runtime) and "loader" are kept as English loanwords; "Saves", "Logs", "Mods",
  "Backups", "modpack", "template", "perks", "cheats", "single-player", "co-op", "host" too. Say if a Portuguese word reads better.
- "chave" for the on/off switch control: could be "interruptor" or just "botão".
- "deu passagem" for "stepped aside" (another loader takes over): alternative "ficou de lado". "passagem de início"
  (ShroudForge's startup pass) sits close to it and is awkward.
- "processar" for "cook" (Kiln makes a mod's models, animations and sounds from the game).
- "modo seguro ocioso" for "safe idle".
- "painel de desempenho" for the graphics readout (it shows frame times and the upscaler in use).
- "Fog of war" is "névoa de guerra", near "Névoa" (Shroud); "a névoa da Névoa" in `admin.*flameLevelFogRemovalHint` is awkward.
- Compass letters on the admin map are Brazilian: N, S, L (east), O (west), NE, NO, SE, SO.
- "Gerar" for the server Spawn page; players may just say "spawn".
- "ponto de referência" for waypoint is long; "waypoint" may suit players better.
- Game `catalog.check.type`: `kind.*` values carry an article ("um número") to fit "{setting} aceita {type}, não {given}".
- Game `sf.other_side` style texts assume `{sides}` is masculine ("só no cliente").
- `admin.spawn.removeNone`: English has a double space (probably an empty `{n}`); written with one.
- "Creative Mode" (the Flame mod's name) kept; Kiln's own feature is "modo criativo".
- Steam / Steam Deck menu names were written from memory and should match Steam's real pt-BR UI: Energia,
  Alternar para a Área de Trabalho, Adicionar um jogo não Steam à biblioteca, Opções de inicialização, Propriedades,
  Arquivos instalados, "Verificar integridade dos arquivos do jogo", Modo de Jogo. Deck mode is "Modo Deck".
- Release channel name "Estável"; "Builds de desenvolvimento" for dev builds.
- Load phase ids Dllmain / Early / Boot / World kept in English (they are ids); sentences say "fase early".
- Hook conflicts: Overlap "Conflito", Hooked first / over "Hook antes" / "Hook por cima"; Load late "Carregar por último".
- `mods.order.loadsNum` reads "carrega em {num}º." ({num} is the row's position).
- Nexus: Endorsements "Endossos", Most endorsed "Mais endossados".
- Tester (Discord role) "Testador", Volunteer "Ser testador"; News "Novidades".
- Keys: Unbind "Desatribuir" (maybe "Remover tecla"); Bindings "Atalhos"; profiling "Medir desempenho".
- Talents: Buff "bônus", Refund "Reembolsar". Gem Forge "Forja de Gemas". Guided checks "Verificações guiadas"
  (PASSOU / FALHOU / PULADA); achievement guard "Proteção de conquistas".
- Server list placeholder "Ember Isles" became "Ilhas de Brasa". Game look: "Visual clássico do Kiln (brasa)".
- Network in/out: "recebidas/enviadas"; `clients.col_frames` "Quadros receb./env.".
- `X mods on, Y off` texts are written "X mods ativados, desativados: Y" so a Y of 1 still reads right.
- `deck.home.sfModSkipped.one`: English "one mod" has no `{count}`, so the pt-BR `one` says "um mod" (CLDR `one`
  also covers 0, which this text likely never shows).
- English typos left out: `servers.head.stoppedKiln` / `head.stopped` have a space before a comma.

## Labels over about 130% of English
- admin: "Salvar ponto de referência", "Pontos de referência", "Nível da Chama que remove a névoa".
- game: "Só o que já encontrei", "Mostrar o painel de desempenho", "O QUE ELE ESPERA", "CHAVE E CONFIGURAÇÕES" (about 120-140%).
- launcher: "Estatísticas de tick" (Tick stats), "Configurações de mods" (server nav Configs, to tell it from Settings),
  "Ativar mostrados ({count})" / "Desativar mostrados ({count})" (Shown on/off), "Ordem de carregamento" (Load order),
  "Predefinição de dificuldade", "Verificação desativada", "Builds de desenvolvimento".
