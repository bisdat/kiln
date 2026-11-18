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

## The contract with the relay

The relay runs on frostdev.io (the kilnbot repo; its own contract is `docs/relay-api.md` there). Both sides build to
PLAN.md section 7.2; this is what the launcher sends and expects. Paths and shapes the relay settles differently are
reconciled by the owner.

**Base address:** `https://frostdev.io/kiln/relay` by default. It is a setting (`relay` in `<data>/community.json`);
only https on frostdev.io or one of its subdomains is used, so a staging relay can be tried and the session key never
goes to another site. Debug builds can send the same requests in plain HTTP to a relay on this machine with
`KILN_COMMUNITY_RELAY_LOOPBACK=127.0.0.1:<port>` (the `Host` header stays `frostdev.io`); release builds ignore it.

**Every request:** https to that one host through `kiln_manage::net` (no redirects followed, public addresses only, no
proxy, no cookies, bounded answers: 256 KiB, the known-problems list 512 KiB). Headers: `User-Agent:
kiln-launcher/<version>` (nothing else about the player or the PC), `Accept: application/json`, and for the calls
marked "session" `Authorization: Bearer <session key>`. JSON bodies are `Content-Type: application/json`, UTF-8.
Field names are camelCase.

**Errors:** any non-2xx answer, with a JSON body `{"error": "<reason>", ...}` where the launcher reads one. The
launcher turns them into the error kinds the UI words (`detail.kind`):

| Answer | Kind | What the launcher does |
|---|---|---|
| no answer (DNS, refused, reset, timeout before connecting) | `offline` | "You're offline"; the draft is kept |
| 5xx, a body that is not the shape below, a redirect, an answer over its bound | `relay` | "Kiln support can't be reached"; Retry |
| 401 | `signin` | forgets the session key (signed out) |
| 403 `{"error": "not_member"}` | `not_member` | "You're not in the Kiln Discord yet", Join and "I've joined" |
| 403 `{"error": "too_young"}` | `too_young` | the account is under 3 days old (shown as the relay's refusal) |
| 403 any other reason (a blocked account) | `relay` | the same words as a relay that is down (PLAN 7.3) |
| 404 | `not_found` | the post or the attempt is gone |
| 410 | `expired` | a sign-in that ran out |
| 413 `{"error": "too_big", "size": n, "limit": n}` | `too_big` | "The report is too big to send", Send without the zip |
| 429 `{"error": "rate_limited", "sent": n, "waitMin": n, "last": "<post url>"}` | `rate_limited` | "That's a lot of reports", Open my last post (`last` only if it is a `https://discord.com/channels/...` link) |
| 400 | `bad_request` | a launcher bug; shown as the relay's refusal |

**Shapes used below:**

- `Account` = `{id: "<snowflake>", name: string (1 to 64), member: boolean, tester: boolean}`.
- `SupportPost` = `{id: [A-Za-z0-9_-]{1,64}, title (≤120), url: "https://discord.com/channels/<guild>/<thread>",
  created: RFC 3339, status: "waiting"|"replied"|"solved"|"closed", replies: n, unread: n (replies the player has not
  seen in the launcher), lastReply: {name, team: boolean (Kiln Team role), at: RFC 3339} | null, tags: string[] (≤8),
  zip: "team"|"public"|"none"}`. A post that breaks these limits is left out of the list.
- Ids that go into a path are `[A-Za-z0-9_-]{1,64}`; the launcher refuses anything else before sending.

### Sign-in (no session)

| Method, path | Request | Answer |
|---|---|---|
| `POST /signin` | `{mode: "browser"\|"qr", join: boolean, challenge: base64url(sha256(secret)) without padding, port?: number (browser only: the loopback port), device: "pc"\|"deck", version: "<launcher version>"}` | `{id, code: string ("HV3R-9TCW"; empty in browser mode), url: https on frostdev.io (browser: the page the browser opens, e.g. `/kiln/signin/<id>`; qr: the phone link, e.g. `/link/HV3R9TCW`), expiresAt: RFC 3339}` |
| `POST /signin/exchange` | `{id, code: "<one-time code from the loopback>", verifier: "<secret>"}` | `{session: "<key>", account: Account}` |
| `POST /signin/poll` | `{id, secret: "<secret>"}` | `{state: "waiting"}` \| `{state: "approved", session, account: Account}` \| `{state: "denied"\|"expired"\|"canceled"}`; 404 or 410 also read as expired |

- The secret is 32 random bytes in base64url (43 characters); the challenge is its SHA-256 in base64url. The session
  goes only to whoever proves the secret.
- **Browser (desktop):** the launcher listens once on `127.0.0.1:<random port>` before `POST /signin`, then opens
  `url` in the system browser. After Discord, the relay sends the browser to
  `http://127.0.0.1:<port>/done?id=<id>&c=<one-time code>`, or `...&error=canceled|denied|expired` when it did not
  finish. The launcher answers that request with a small page ("Signed in. You can close this tab and go back to the
  launcher.") and calls `/signin/exchange`. Any other request on the port (another path, another id, another `Host`
  than `127.0.0.1:<port>`, a repeated parameter) gets a 404 and changes nothing. If the redirect never arrives (a
  firewall), the launcher also polls `/signin/poll` from 8 seconds on, every 4 seconds: the relay may answer
  `approved` there too.
- **QR (Deck):** no port; the Deck shows `url` as a QR code and `code` as text, and polls `/signin/poll` at most every
  2 seconds until an ending. An attempt lasts at most 10 minutes on the launcher's side.
- The session key the launcher keeps is printable ASCII without spaces, 16 to 512 characters.

### The account (session)

| Method, path | Request | Answer |
|---|---|---|
| `GET /me` | - | `{account: Account}` |
| `DELETE /session` | - | 2xx (sign out: the relay deletes the session) |
| `DELETE /me?posts=0` / `DELETE /me?posts=1` | - | 2xx ("Sign out and forget me"; `posts=1` also deletes the player's support posts on Discord) |

Sign out always deletes the key on this PC, also when the relay can't be reached. "Forget me" reports a failure (it
must reach frostdev.io), but the key is gone either way.

### Reports and posts (session)

| Method, path | Request | Answer |
|---|---|---|
| `POST /reports` | `multipart/form-data`: part `draft` (`application/json`, the object below) and, unless the player chose Nobody, part `zip` (`application/zip`, `filename="kiln-report-YYYY-MM-DD.zip"`, at most 8 MiB; the launcher refuses a bigger one before sending) | 2xx `{post: SupportPost}` |
| `GET /posts` | - | `{posts: SupportPost[]}` (at most 200 are read) |
| `POST /posts/{id}/seen` | - | 2xx (the player opened it: its replies are seen) |
| `POST /posts/{id}/solved` | - | 2xx ("It's fixed": the Solved tag) |
| `DELETE /posts/{id}` | - | 2xx ("Delete my post": the thread, and the team's copy of the zip) |

The `draft` part (filled in by the send step; see "Send to Kiln support" below):
`{title, kind: "noload"|"mod"|"crash"|"launcher"|"server"|"other", when?: "start"|"menu"|"load"|"play"|"join"|"other",
words, zip: "team"|"public"|"none", from: "logs"|"banner"|"crash"|"community"|"deck", known?: "<known problem id>",
post: PublicPost}` where `PublicPost = {title, tags: string[], kind, words, findings: [{id, level:
"problem"|"warning"|"info", text}], versions: [["Kiln", "0.2.0-dev.29.18"], ["Launcher", ...], ["System", ...], ["Runs
on", ...], ["Game build", ...], ["Mods", "12 on"]]}`: exactly what the check step showed.

### Public (never a session, even when signed in)

| Method, path | Answer |
|---|---|
| `GET /feed` | `{releases: [{title, date: RFC 3339, line, url: https}], events: [{title, start: RFC 3339, end?: RFC 3339, where: string, url: https}]}`; the launcher shows at most 3 releases and 5 events |
| `GET /known-issues` | the known-problems list (format below) |

### Testers (session)

| Method, path | Request | Answer |
|---|---|---|
| `GET /tester` | - | `{tester: boolean, plan: Plan \| null, sent: SentMark \| null}` (the current test build's plan and this tester's results for it) |
| `POST /tester` | `{hardware: {gpu, driver, vram, os, runner, covers: ("fsr4"\|"xess"\|"framegen"\|"deck")[]}, beta: boolean}` | 2xx (the Tester role is given) |
| `DELETE /tester` | - | 2xx ("Stop testing": the role and the hardware go) |
| `POST /tester/results` | `multipart/form-data`: part `results` (`application/json`: `{build, steps: [{id, result: "worked"\|"failed"\|"skipped", note}], hardware}`) and, when "Attach a support report" is ticked, part `zip` as for reports | `{sent: SentMark}` |

`Plan = {build: "0.2.0-dev.30 Beta 2", url: "<the build's #testing post>", until: RFC 3339, steps: [{id, title, how:
string[], for: {label: "AMD RX 6000 and newer", needs: Capability[]} | null}]}`; `SentMark = {at, read: {by, at} |
null, replied: boolean}`.

## What the launcher keeps on this PC

| File (data folder) | What | Until |
|---|---|---|
| `community.json` | `{format: 1, shown, presence, notify, relay?}` | the player changes it |
| keyring entry, or `community-session.json` (0600) | `{format: 1, relay: "<host>", session, account}` | sign out |
| `community-known.json` | the last known-problems list (public) | replaced at the next page open |

The account and the posts as last read stay in memory while the launcher runs, so the page can say "as of 11:47" when
offline. Nothing about the player's Discord account stays after sign-out.
