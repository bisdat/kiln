# Korean (ko): notes for reviewers

Kiln's Korean texts (한국어) came in 0.2.0-dev.29.19 as a **machine translation** (made with Claude, an AI model),
waiting for native speakers to check and improve them: [the review issue](https://github.com/frostdev-ops/kiln/issues/98). The files are
[`launcher/src/i18n/ko.json`](../../../launcher/src/i18n/ko.json) (the Kiln Launcher) and
[`runtime/locales/ko.json`](../../../runtime/locales/ko.json) (Kiln's texts in the game);
[translating.md](../translating.md) has the format and how to test a change without building anything.

Below: the choices the translation made, its glossary, and what a native speaker should look at first.

Files: `launcher/src/i18n/ko.json` (4,010 texts) and `runtime/locales/ko.json` (805 texts). Both are complete: no text
is left in English except product, tool, file and key names, which stay as they are by rule.
language.name: 한국어. Plural forms: `other` only.

## Address form
- Sentences use formal polite 합니다체 everywhere (…합니다, …습니다). Requests use …하세요, questions in confirm
  titles use …할까요? (e.g. "Kiln을 제거할까요?"). No 해요체, no 반말.
- Labels (buttons, tabs, menu entries, column heads, chips, Deck entries) are short nouns or noun + verb stem with no
  ending ("설치", "다시 시도", "업데이트 확인"). States are short too: 켜짐 / 꺼짐 / 실행 중 / 대기 중 / 완료 / 실패.
- Particles after a placeholder are written as 을(를), 이(가), 은(는), (으)로, since the value is unknown. Where the
  value is a Kiln version, "버전" is added after it instead (Kiln {version} 버전이 ...).

## Glossary (English -> Korean)
| English | Korean | English | Korean |
|---|---|---|---|
| mod | 모드 | modpack | 모드팩 |
| profile | 프로필 | runtime | 런타임 |
| loader | 로더 | engine / engine plugin | 엔진 / 엔진 플러그인 |
| plugin | 플러그인 | native mod | 네이티브 모드 |
| launcher | 런처 | server / dedicated server | 서버 / 전용 서버 |
| client | 클라이언트 | world | 월드 |
| save / save file / Saves tab | 세이브 / 세이브 파일 / 세이브 | backup / restore | 백업 / 복원 |
| support report / report | 지원 보고서 / 보고서 | crash report | 충돌 보고서 |
| test build | 테스트 빌드 | channel / beta / release | 채널 / 베타 / 릴리스 |
| Library / Catalog | 라이브러리 / 카탈로그 | Community | 커뮤니티 |
| Deck mode | 덱 모드 | Editor / save editor | 편집기 / 세이브 편집기 |
| Play / About / Logs | 플레이 / 정보 / 로그 | Settings | 설정 |
| game folder / data folder | 게임 폴더 / 데이터 폴더 | load order / phase | 로드 순서 / 단계 |
| phases dllmain, early, boot, world | Dllmain, 초기, 부팅, 월드 | safe idle | 안전 대기 |
| config(s) | 구성 | setting(s) | 설정 |
| switch on / off; on / off | 켜기 / 끄기; 켜짐 / 꺼짐 | install / uninstall Kiln | 설치 / 제거 |
| hook | 후크 | proxy | 프록시 |
| cook (asset cooking) | 쿡 (쿡 중, 다시 쿡) | tick | 틱 |
| overlay | 오버레이 | upscaler | 업스케일러 |
| frame cap | 프레임 제한 | frame generation | 프레임 생성 |
| readout (graphics) | 정보 표시 | render scale / preset | 렌더링 배율 / 렌더링 프리셋 |
| creative mode / menu | 크리에이티브 모드 / 메뉴 | Item details | 아이템 정보 |
| guided checks | 안내 점검 | talent tree | 특성 트리 |
| skill points / skill tree | 스킬 포인트 / 스킬 트리 | respec | 스킬 초기화 |
| Gem Forge / gem | 보석 대장간 / 보석 | ability | 능력 |
| spawn | 스폰 | entity | 엔티티 |
| waypoint / zone | 웨이포인트 / 구역 | protection zone | 보호 구역 |
| teleport | 순간이동 | god mode | 무적 모드 |
| admin | 관리자 | permission node | 권한 노드 |
| the Flame / Flame level | 불꽃 / 불꽃 레벨 | the Shroud | 안개 |
| base | 기지 | survivors (roster) | 생존자 (명단) |
| crafting station | 제작대 | creature | 생물 |
| tombstone / grave | 묘비 / 무덤 | perk | 특전 |
| prefix (Wine/Proton) | 프리픽스 | sign in / out | 로그인 / 로그아웃 |
| tester | 테스터 | known problem | 알려진 문제 |

Confirm words (common.confirmWord.*): clean 정리, delete 삭제, forget 잊기, restore 복원, switch 전환, uninstall 제거.

## Please check
- **Game terms.** I did not have Enshrouded's official Korean strings. Please compare: the Shroud (안개), the Flame and
  Flame level (불꽃 / 불꽃 레벨; Flame Altar and Embervale do not appear in these texts), crafting station (제작대),
  survivors roster (생존자 명단), perk (특전), tombstone (묘비), glider (글라이더), body heat (체온), stamina (기력),
  rarities (일반 / 고급 / 희귀 / 영웅 / 전설), the server-settings labels in `admin.gameSettings.*` (e.g. "Startling a
  tame" = 길들인 동물 놀래키기, "Threat bonus" = 위협 보너스, "Pacify all enemies" = 모든 적 진정).
- **The flame wordplay.** English uses "lit / put out / light it" for mods switched on and off (servers.mods.*,
  ui.form.flame*, profiles.modList.lit). Korean has no natural equivalent, so these are plain 켜짐 / 꺼짐 / 켜기 /
  끄기, and "The flame decides what loads" became "스위치가 정합니다".
- **"cook"** is kept as the loan 쿡 (쿡 중, 쿡 완료, 다시 쿡). A native speaker may prefer 빌드 or 변환.
- **Phase names.** dllmain is left as Dllmain (a code name); early, boot, world are 초기, 부팅, 월드.
- **Deck tab "Kiln", "Rich Presence", "Mod Manager Download", "Enshrouded Interactive Map"** stay in English (product
  or Discord/Nexus feature names).
- **Developer and log-like lines** in the in-game checks (`checks.assert.*`: "ok", "FAIL", the "cheats" tag) stay as
  they are, as they read like log output.
- **Server-browser example name** `servers.list.namePlaceholder` "Ember Isles" became 불씨 군도 (a free rendering).
- **Placeholder sample** `admin.players.waypointPlaceholder` "Home base" = 본거지, `admin.zones.namePlaceholder`
  "Main base" = 본 기지, `saveBackups.notePlaceholder` "before the boss" = 보스전 전에.
- **Privacy texts** (`community.privacy.*`, `community.signIn.*`) were translated closely; please read them once more
  for accuracy, since they describe what is sent and kept.
- **Graphics.** FSR and XeSS are never called DLSS, and no word for "unlock" (잠금 해제) is used about upscalers.
  (해금 is used only for talents and item upgrades in the creative menu.)

## Labels that stay long
Korean labels are mostly at or under half the English character count. A few stay longer because they carry a
placeholder or a product name ("{name}을(를) 내 모드에 추가", "Nexus Mods에서 업데이트", "보이는 것 켜기 ({count})",
"아직 Kiln으로 실행한 적 없음"), and confirm-dialog titles are full questions ("모드 {count}개를 제거할까요?").
None should overflow more than the English does.
