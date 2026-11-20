# Japanese (ja): notes for reviewers

Kiln's Japanese texts (日本語) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/95). The files are
[`launcher/src/i18n/ja.json`](../../../launcher/src/i18n/ja.json) (the Kiln Launcher) and
[`runtime/locales/ja.json`](../../../runtime/locales/ja.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: `launcher/src/i18n/ja.json` (4,010 texts), `runtime/locales/ja.json` (805 texts). Every text is translated.
Plural forms: `other` only (CLDR Japanese). `language.name` = 日本語.

## Address form and style
- Sentences (hints, notices, dialogs) use polite です/ます. 「あなた」 is used only where the sentence needs it.
- Labels (buttons, tabs, menu entries, column heads, Deck entries) are short nouns or verb stems with no
  「〜してください」: 保存, 削除, 更新を確認, モッドを追加.
- Questions in dialog titles end in 「〜しますか？」. Toasts are past tense: 「〜しました」.
- In-app place names are in 「」 when the text points at them: 「プレイ」, 「設定」の「プロファイル」, 「ランタイム」.
- Counters have a space after a number placeholder: 「{count} 個のモッド」, 「{n} 件」, 「{count} 人」.
- Parentheses: full-width （） in Japanese running text; ASCII () kept where English puts a count or code in a label
  (「注意 ({count})」, 「適用 ({count})」). A reviewer may want one style throughout.

## Glossary (English -> Japanese)
| English | Japanese |
|---|---|
| mod / mods | モッド |
| profile | プロファイル |
| server / dedicated server | サーバー / 専用サーバー |
| launcher | ランチャー (Kiln Launcher kept as a name) |
| runtime (also the Runtime tab) | ランタイム |
| loader | ローダー |
| engine (Kiln's word for a mod engine) | エンジン |
| report / support report / crash report | レポート / サポートレポート / クラッシュレポート |
| modpack | モッドパック |
| world | ワールド |
| save / saves | セーブ / セーブデータ |
| backup | バックアップ |
| test build | テストビルド |
| channel / beta / release | チャンネル / ベータ / リリース（版） |
| update (noun) / update (verb, button) | アップデート / 更新 |
| Library / Catalog | ライブラリ / カタログ |
| install / uninstall / repair | インストール / アンインストール / 修復 |
| roll back | ロールバック |
| cook (build mod assets for this game) | クック, Cook again = 再クック |
| switch on / off, on / off | オンにする / オフにする, オン / オフ |
| lit / put out (Kiln's flame switch) | 点灯 / 消灯; light it = 灯す, put it out = 消す |
| safe idle | 安全待機 (sentences: 安全待機モード) |
| step aside (Kiln yields to another loader) | 控える / 譲る; label 控え中 |
| vanilla | バニラ |
| native mod / plugin | ネイティブモッド / プラグイン |
| load order | 読み込み順 |
| hook | フック |
| overlay | オーバーレイ |
| key binding / unbind | キー割り当て / 解除 |
| upscaler / frame generation / frame cap / readout | アップスケーラー / フレーム生成 / フレーム上限 / 情報表示 |
| game folder / data folder | ゲームフォルダー / データフォルダー |
| config(s) | 設定 / 設定ファイル |
| permissions / node / rule | 権限 / ノード / ルール |
| admin | 管理 (adj.), 管理者 (person) |
| player | プレイヤー |
| waypoint / teleport | ウェイポイント / テレポート |
| protection zone | 保護ゾーン |
| God mode | ゴッドモード |
| creature / enemy / boss | クリーチャー / 敵 / ボス |
| spawn | スポーン |
| tombstone / grave | 墓 |
| the Flame / Flame level | 炎 / 炎のレベル |
| the Shroud | 霧 |
| base | 拠点 |
| survivors (NPC roster) | 生存者 |
| crafting station | 作業台 (also クラフト台 in the overlay debug line) |
| health / mana / stamina / body heat | 体力 / マナ / スタミナ / 体温 |
| skill points / respec | スキルポイント / 振り直し |
| talents (Kiln talent trees) | タレント |
| rarity: common ... legendary | レア度: コモン, アンコモン, レア, エピック, レジェンダリー |
| Gem Forge | ジェムフォージ |
| Creative / creative mode | クリエイティブ / クリエイティブモード |
| Rich Presence | リッチプレゼンス |
| sign in / sign out | サインイン / サインアウト |
| endorsements (Nexus) | 推薦数 |
| Community | コミュニティ |
| Deck mode / Desktop | Deck モード / デスクトップ |
| confirm words | clean 整理, delete 削除, forget 解除, restore 復元, switch 切替, uninstall アンインストール |

Unchanged: Kiln, Enshrouded, ShroudForge, EMF, EML, SET, Flame (the loader), Greeneye, Enshwick, XHL-Core, Shroudtopia,
shroudlink, Ember, OptiScaler, DLSS, FSR, XeSS, Steam, Proton, Wine, GE-Proton, Discord, Nexus Mods, GitHub, Windows,
Linux, Steam Deck, .NET, file names, settings keys, commands, env vars. The native phase names Dllmain, Early, Boot,
World (profiles.native.*) stay English on purpose: they are the phase values a mod's native.json uses.

## Please check (native speaker)
1. **Enshrouded's official Japanese terms.** I did not have the game's ja localization at hand. Unverified:
   霧 (Shroud), 炎 / 炎の祭壇 (Flame, Flame Altar), 拠点 (base), 生存者 (survivors), 作業台 (crafting station),
   墓 (tombstone), 体力 / 体温 (health, body heat), the rarity names, and the server setting names in
   admin.gameSettings.* (飢餓デバフ, パーク, 脅威ボーナス, 呪いの強さ, 飼育動物を驚かせたとき). Please replace any of them
   with the game's own wording; the same terms appear in runtime/locales/ja.json (creative.*, graphics.mode.*).
2. **DLSS mode names** (パフォーマンス, バランス, クオリティ, ウルトラパフォーマンス, ウルトラクオリティ): should match the
   game's graphics menu.
3. **Steam UI names** in deck.help.* (ライブラリ, プロパティ, 一般, 起動オプション, 電源, デスクトップに切り替え,
   Steam 以外のゲームをマイライブラリに追加, 参照, 選択したプログラムを追加, Steam 以外): taken from memory of Steam's
   Japanese client. The Windows path in restoreDataOffer.explain and play.banner.advice* uses
   "ゲームファイルの整合性を確認" for Steam's "Verify integrity of game files".
4. **Confirm words** the player types with an IME: 整理, 削除, 解除, 復元, 切替, アンインストール. 「切替」 (no り/え) was
   picked as the shortest common form; 「解除」 was picked for "forget" (forget a game, forget my Discord data).
5. **Flame switch wordplay** (ui.form.flame*, servers.mods.lit/out, profiles.modList.lit): 灯す / 消す / 点灯 / 消灯.
   Check that it reads naturally and is not confusing next to plain オン/オフ.
6. **Glued fragments**: admin.players.levelOf + atCap + xpOf + xpPending + pointsUnspent + overBudget are joined by
   code; their leading 、/。 were chosen to join cleanly. Same for network.sync.applied + reset + errors and
   admin.saves.restartOnline + restartBody. Please look at them in the running app.
7. **servers.head.stoppedKiln / stopped**: the English has an empty runner slot ("Start runs it , Kiln {kiln}."); the
   Japanese drops the gap.
8. 「控える / 控え中」 for "step aside" and 「譲りました」 for "stepped aside for X": check the tone.
9. Tester / privacy texts (community.privacy.*) are a promise to players; please read them for exact meaning.

## Labels I could not keep at half the English length
Japanese is denser per character, but these labels are about as long as English or longer:
- common.confirmWord.uninstall アンインストール (and runtime.uninstall, deck.kiln.uninstall)
- deck.catalog.filterInstalled / catalog.installed インストール済み
- deck.home.lookingSub, shell.appShell.lookingSub Enshrouded を探しています
- deck.shared.steamSetting, play.banner.setting Steam の設定が必要
- deck.mods.undoKept / mods.list.undoKept / shell.drop.undoKept 元に戻しましたが、後の変更は残しました (a toast title)
- deck.home.sfModsFailed モッドが読み込まれませんでした
- settings.graphics.labels.dlss*Scale DLSS レンダースケール（ウルトラパフォーマンス）
- admin.gameSettings.tamingStartleRepercussion 飼育動物を驚かせたとき
- catalog.mayNotLoad Kiln はこれを読み込めない可能性があります
- community.presence.details Kiln で Enshrouded (shown on Discord profiles)
Most of the remaining "long" labels are toasts or status lines, where a full sentence fits.
