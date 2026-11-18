# Discord in the Kiln Launcher

The launcher's optional Discord features: the Community page (desktop) and the Community tab (Steam Deck), signing in
with Discord through frostdev.io, "Send to Kiln support", the player's support posts and their replies, known
problems, the tester programme, and Rich Presence. Design: `/mnt/Dongus/kiln-wave/dev29/out/discord-prototype/`
(PLAN.md, WIREFRAMES.md, DECISIONS.md). The privacy page that binds all of it: <https://frostdev.io/kiln/privacy>.

Code: `launcher/src/community/` (the UI), `launcher/src/api/commands.ts` (the commands, "community"),
`launcher/src/api/mockCommunity.ts` (the browser mock), `launcher/src-tauri/src/commands/community.rs` and
`launcher/src-tauri/src/community/` (Tauri), `crates/kiln-manage/src/community/` (the logic, tested without Tauri).

## The rules

- **Optional and off by default.** Settings has "Show the Community page" (on by default). Off hides the page, the
  Deck tab, the reply count and every "Send to Kiln support", and the backend refuses every Discord-related request
  before it is sent (`detail.kind` `hidden`). Shown but signed out, with Rich Presence off, the only requests are the
  public news feed and the known-problems list (`GET /feed`, `GET /known-issues`), and only when the page is opened.
- **Nothing leaves the machine without a click** on a screen that shows what goes. A report is prepared, shown (the
  public post as it will look, the zip with its file list, who sees it) and sent as prepared. A tester's hardware is
  shown before Volunteer.
- **No secret in the launcher.** The only key the launcher holds is the Kiln session key the relay hands out at
  sign-in. It is kept in the system keyring (Linux Secret Service through `secret-tool`; Windows Credential Manager),
  else in `<data>/community-session.json` readable by the user only. Never a Discord token, a bot token, a webhook URL
  or a client secret. The application's client ID (`1570849158431244459`) is public: it names the app for Rich
  Presence.
- **Rich Presence** talks only to the Discord app on this PC (its local IPC socket), only while the switch is on and the
  game runs with Kiln. Nothing goes to Frostdev for it. It never shows a server, world, character or mod name.

## What it does

**Where it lives.** Desktop: "Community", the last word of the header (after Servers), with a flame numeral for new
replies from Kiln support. Deck: a Community tab between Editor and Help, with the same numeral. Both are hidden, with
every "Send to Kiln support", while Settings' "Show the Community page" is off (desktop Settings, Launcher; Deck
Settings). The page and tab: Get help, your support posts, the tester programme, news and events, the Discord account,
Rich Presence with its preview, links, and "What is sent and kept".

**Sign in with Discord** (always through frostdev.io; the launcher never holds a Discord token). Desktop: the dialog
says what Kiln gets ("Also join the Kiln Discord" ticked by default), the system browser opens the relay's page, the
dialog waits and moves on by itself; endings: signed in (a toast), cancelled on Discord's page, timed out after 10
minutes, offline, frostdev.io not answering, each with Try again. Deck: a QR code and a short code for a phone, the time
left, X for a new code; expired, declined, offline. Sign out has no confirmation; "Sign out and forget me" takes the
typed word on the desktop (a tick also deletes the player's support posts from Discord) and a hold on the Deck. The
page re-reads the account and membership when it opens.

**Send to Kiln support.** Entry points: the Logs page's support report row, Play's problem banners after the fix they
already offer (Kiln is not loading, Kiln did not start, Kiln update failed, No mods loaded), ShroudForge's failed
startup pass on Engines, the crash notice (a banner on Play after the game closed while Kiln wrote a crash report in
that run; a line on Deck Home), the Community page and the Deck's tab and Home. Each preselects a kind. Desktop: write
(one line of up to 90 characters, the kind, up to 1500 more), check what is sent (the public post as it will look, the
zip's size and files, who sees the zip: the Kiln team only, everyone in #support, or nobody; what is never in it), sent
(Open in Discord). Deck: pick what went wrong and when, check and send (Type asks Steam for its keyboard), sent with a
QR code. The report is the existing support report (`kiln_manage::report`), built once when the dialog opens and sent
as built. Its public post (`kiln_manage::community::post`) holds the player's words, the kind, the report's findings
and the versions: Kiln, the launcher, the system and the Proton or Wine build, the game's build, how many mods are on.
Never the mod list: a mod is named only when a finding names it. Every error keeps the draft (not signed in: the
sign-in comes back to the check step; not in the server: Join and "I've joined"; offline: Save report; frostdev.io down:
Retry; too many reports: when again and the last post; too big: Send without the zip). A draft with words survives
closing the dialog.

**Known problems** (`known-issues`, format in `kiln_manage::community::known`): the launcher keeps the last copy of
the public list, fetched with the news when the Community page opens, and matches it against the report's findings on
this PC. A match shows its fix first (a line to copy, or a button to the mods, Runtime or Logs) with "If this fixes it,
there's nothing to send"; sending stays possible.

**Your support posts**: Waiting, New reply (unread, flame), Replied, Solved, Closed; who replied last and when ("of the
Kiln team" for the Kiln Team role); Open in Discord (the Deck: a QR code); `···` Copy link, It's fixed (the Solved
tag), Delete my post (typed on the desktop, held on the Deck). Opening a post marks its replies seen. The list is read
when the page opens and every 5 minutes while signed in with the page shown. "Tell me when Kiln support replies" (off
by default) adds a system notification with Open in Discord: `notify-send` (or the notification service through
`gdbus`) on Linux, a toast through PowerShell on Windows.

**Testers.** "Help test Kiln" says what this PC can test (FSR 4 on AMD RX 6000 and newer, XeSS on Intel Arc, frame
generation on NVIDIA RTX 40 and 50, the Steam Deck). Volunteer shows first exactly what is shared: graphics card,
driver, video memory, system, how the game runs (`kiln_manage::community::hardware`: sysfs and pci.ids, Mesa's library
name, NVIDIA's version file and `nvidia-smi` on Linux; the display adapter's registry key on Windows; nothing needs new
privileges). A tick (on) switches the launcher to test builds through the existing channel switch. A tester sees the
current test build's checklist with only this hardware's steps ("2 steps are for other hardware"), sets Worked,
Didn't work or Couldn't try with one line per step (on the Deck a pick for what happened), and sends the results
(with a support report for the team only, ticked by default) once every step has one. The team's check mark shows as
"read". Stop testing is in the checklist's `···` (the Deck: X).

**Rich Presence** (`kiln_manage::community::presence`, `launcher/src-tauri/src/community/presence.rs`): one switch,
off by default, with the preview card. While a client game runs with Kiln (its boot.json from this run), the switch is
on and the page is shown, the Discord app on this PC shows "Playing Kiln / Enshrouded with Kiln / N mods on / elapsed"
with Get Kiln and Join the Discord. Never a server, world, character or mod name. The socket: Windows
`\\.\pipe\discord-ipc-N`; Linux `discord-ipc-N` in `$XDG_RUNTIME_DIR` and its Flatpak (Discord, Vesktop) and Snap
folders, then `$TMPDIR`, `$TMP`, `$TEMP`, `/tmp`. Art keys `kiln-hearth` and `kiln-mark` (an activity Discord refuses
for its art goes again without it). Its own thread follows the watcher, tries the socket every 30 seconds while the game
runs, and clears the activity at once when anything ends; it never holds up a start. "Discord isn't running on this
PC" (or Deck) when the switch is on and no socket is there.

**Mock mode** (`?mock=1&discord=<flags>`, `src/api/mockCommunity.ts`): `in`, `empty`, `notmember`, `offline`,
`relaydown`, `ratelimit`, `toobig`, `known`, `nodiscord`, `presence`, `notify`, `cancel`, `expire`, `deny`, `tester`,
`sent`, `read`, `noplan`, `hidden`; `&problem=crash` (the game runs four minutes and closes with a crash report) or
`&problem=nokiln` (the next Play starts without Kiln); `__kilnMock.community.{reply, approve, setOnline, setRelay,
join, readResults, requests}` and `__kilnMock.crash()`.

## Privacy

What the launcher does stays within <https://frostdev.io/kiln/privacy> (published 18 November 2026). "What is sent
and kept" says it in short; `unit/community.test.ts` checks that its words keep the page's promises and times.

- Nothing about the player is sent until they press a button on a screen that shows what goes. Signed out, the only
  requests are the public news and the known-problems list when the page opens (`tests/community.spec.ts` checks the
  mock's record of relay calls; the backend refuses everything while the page is hidden).
- Sign-in: Kiln gets the Discord ID, name and picture; frostdev.io keeps a fingerprint of the session key (90 days
  unused), the name and picture while signed in. The launcher keeps the session key (keyring, else an owner-only file)
  and the account as last read; sign out removes both here whatever the relay says.
- Reports: the public post as shown; the zip to the team (deleted from their channel after 30 days), to everyone, or
  not sent. Not sent means report.json alone goes to the relay instead, holding only what the post shows (the
  launcher's version, the findings, the versions line's facts), so the relay can write the post. The zip is redacted
  by `kiln_manage::report` (user, machine and Steam names, Steam ID, IP addresses, the home folder in paths); saves and
  mod settings are never in it.
- Testers: the hardware lines shown, sent by Volunteer only; results to the build's post; an attached report to the
  team only. Stop testing removes the role and the hardware.
- Rich Presence: only to the Discord app on this PC, nothing to Frostdev.

## The contract with the relay

The relay runs on frostdev.io (the kilnbot repo). Its contract is the relay's own: `docs/relay-api.md` and the 36
examples of `docs/relay-contract.json` there (master 3eb30a7, live since 18 November 2026). This is how the launcher
uses it. The launcher's tests talk to a fake relay on 127.0.0.1, never to frostdev.io.

**Base address:** `https://frostdev.io/kiln/relay`. It is a setting (`relay` in `<data>/community.json`); only https on
frostdev.io or one of its subdomains is used, so a staging relay can be tried and the session key never goes to
another site. Debug builds can send the same requests in plain HTTP to a relay on this machine with
`KILN_COMMUNITY_RELAY_LOOPBACK=127.0.0.1:<port>` (the `Host` header stays `frostdev.io`); release builds ignore it.

**Every request:** https to that one host through `kiln_manage::net` (no redirects, public addresses only, no proxy,
no cookies, bounded answers: 256 KiB, the known-problems list 512 KiB). Headers: `User-Agent: kiln-launcher/<version>`,
the release version About shows, such as 0.2.0-dev.29.18, as at sign-in (nothing else about the player or the PC), `Accept: application/json`, and for the session's calls `Authorization:
Bearer <session key>`. JSON bodies are `Content-Type: application/json`.

**Errors** (`{error, message, ...}`; the launcher reads `error`, never `message`) become the kinds the UI words
(`detail.kind`):

| Answer | Kind | What the launcher does |
|---|---|---|
| no answer (DNS, refused, reset, timeout before connecting) | `offline` | "You're offline"; the draft is kept |
| 503 `unavailable`, 5xx, 422 `bad-zip`, 409, 403 `refused`, a body that is not JSON, a redirect, an answer over its bound | `relay` | "Kiln support can't be reached"; Retry |
| 401 `signed-out` | `signin` | forgets the session key (signed out) |
| 403 `not-member` | `not_member` | "You're not in the Kiln Discord yet", Join and "I've joined" |
| 403 `too-new` | `too_young` | shown as the relay's refusal |
| 404 `not-found` | `not_found` | the post is gone |
| 410 `expired` | `expired` | a sign-in that ran out |
| 413 `too-big` (`size`, `limit`) | `too_big` | "The report is too big to send", Send without the zip |
| 429 `rate-limited` (`limit` hour/day/network, `retryAt`, `lastPost`) | `rate_limited` | "That's a lot of reports", worded for the limit (`detail.limit`): 3 in the last hour, 10 in the last 24 hours, or too many from the network in the last 24 hours; the time until `retryAt` (minutes, else hours); Open my last post (`lastPost` only if it is a `https://discord.com/channels/...` link) |
| 429 `slow-down` | (sign-in poll: still waiting) | |
| 400 `bad-request` | `bad_request` | a launcher bug; shown as the relay's refusal |

Reasons are read with dashes or underscores. Ids that go into a path are `[A-Za-z0-9_-]{1,64}` (the relay's are
Discord snowflakes); the launcher refuses anything else before sending.

**Endpoints, as the launcher calls them:**

| Method, path | The launcher sends | It reads |
|---|---|---|
| `POST /signin` | `{mode: "browser"\|"qr", join, challenge, port? (browser), device: "desktop"\|"deck", version}` | `{id, code, url (https on frostdev.io), expiresAt}` (`pollEvery` ignored: the launcher polls every 2 s on the Deck, every 4 s from 8 s on the desktop) |
| `POST /signin/exchange` | `{id, code (the loopback's c), verifier (the secret)}` | `{token, account}` |
| `POST /signin/poll` | `{id, secret}` | `{state: waiting\|approved (token, account)\|denied\|canceled\|expired}` |
| `GET /me` | | `{account: {id, name, username, avatar, avatarUrl, member, tester}}` (id, name, member, tester are used) |
| `DELETE /session` | | `{ok}` |
| `DELETE /me?posts=1` / `?posts=0` | | `{ok, forgotten}` |
| `POST /reports` | multipart: `draft` `{title (3 to 90), kind, when?, text? (1500, 40 lines), zip: team\|public\|none, known?}` and either `zip` (the report zip, file `kiln-report-YYYY-MM-DD.zip`) or, with `zip: none`, `report` (report.json alone, file `report.json`: `{format, written, tool, findings, versions}`) | `{post, tags, reportId, known}` (post used) |
| `GET /posts` | | `{posts: [Post], unread}` (posts used; at most 200) |
| `POST /posts/{id}/seen`, `POST /posts/{id}/solved` | no body | `{post}` (not used: the list is read again) |
| `DELETE /posts/{id}` | | `{ok}` |
| `GET /feed` (no session) | | `{releases: [{title, version, date, line, url}], events: [{title, when, where, url}], updated}`; at most 3 releases and 5 events; an event without `when` or `url` is not shown |
| `GET /known-issues` (no session) | | `{issues, updated}` (format: `kiln_manage::community::known`) |
| `GET /tester` | | `{tester, hardware, results: [{build, sentAt, url, read, replied}]}`; a `plan` beside them is read when the relay serves one |
| `POST /tester` | `{hardware: {gpu, driver?, vram?, os?, runner?, covers: ["FSR 4", ...]}, beta}` (the lines the volunteer screen showed; a fact it could not read is left out, the card then says "Not known") | answer not used |
| `DELETE /tester` | | answer not used |
| `POST /tester/results` | multipart: `results` `{build (the build's version), steps: [{id, title, result, note?}]}` and, when ticked, `zip` | `{result: {build, sentAt, url, read, replied}}` |

`Post` = `{id, title, url: "https://discord.com/channels/<guild>/<thread>", created, status:
waiting|replied|solved|closed, replies, unread, lastReply: {name, team, at} | null, zip}`; a post that breaks the
launcher's limits (title 120, a Discord link) is left out of the list.

**The desktop sign-in round trip:** the launcher listens once on `127.0.0.1:<random port>` before `POST /signin` and
opens `url` in the system browser. After Discord the relay sends the browser to `http://127.0.0.1:<port>/done?c=<one-time
code>` (an `id` beside it must match when there is one; `?error=canceled|denied|expired` ends the attempt). The
launcher answers that request with a small page ("Signed in. You can close this tab and go back to the launcher.")
and calls `/signin/exchange`. Any other request on the port (another path or id, another `Host` than
`127.0.0.1:<port>`, a repeated parameter) gets a 404 and changes nothing. With a port, the relay gives the session only
to the exchange; the launcher's poll from 8 seconds on only reads "waiting" then.

**The public post** is built by the relay from report.json (the zip's, or report.json alone): the player's line and
words, the kind, the findings (problems, then warnings, then notes), the versions line from report.json's `versions`
object (`{kiln, launcher, os, runner, gameBuild, modsOn, deck}`, written by `kiln_manage::report`), where the zip went,
and the player's mention. The check step draws the same from the same report (`kiln_manage::community::post`): the
relay's versions line ("Kiln 0.2.0-dev.29.18 · Launcher 0.2.0-dev.29.18 · Arch Linux · GE-Proton10-3 · game build
26404966 · 12 mods on") and its tags (the kind's, then Windows, Linux, or Steam Deck and Linux).

## What the launcher keeps on this PC

| File (data folder) | What | Until |
|---|---|---|
| `community.json` | `{format: 1, shown, presence, notify, relay?}` | the player changes it |
| keyring entry, or `community-session.json` (0600) | `{format: 1, relay: "<host>", session, account}` | sign out |
| `community-known.json` | the last known-problems list (public) | replaced at the next page open |
| `community-tester.json` | a tester's results for the current test build `{build, steps: {id: {result, note}}}` | the next build, Stop testing, sign out |

The account and the posts as last read stay in memory while the launcher runs, so the page can say "as of 11:47" when
offline. Nothing about the player's Discord account stays after sign-out.
