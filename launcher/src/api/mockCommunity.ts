// Browser mock of the Discord features (community_* and tester_* commands; ./mock.ts registers them). The relay,
// Discord and the Discord app are all faked here: no network, nothing real. The shapes are the commands' own
// (./commands.ts), so the UI cannot tell this from the launcher.
//
// The URL picks the case:   ?mock=1&discord=<flags>   (comma separated)
//   (none)      signed out          in          signed in, in the Kiln Discord, three posts, one new reply
//   empty       signed in, no posts  notmember   signed in, not in the Kiln Discord yet
//   offline     no network           relaydown   frostdev.io does not answer
//   ratelimit   the next send is refused (3 reports this hour)   toobig   the report zip is over the limit
//   ratelimitday   the next send is refused (10 in 24 hours)     ratelimitnet   (too many from this network)
//   toonew      the Discord account is under 3 days old (a send is refused)
//   signinlimit the relay takes no more sign-ins from this network for now
//   known       the report matches a known problem with a fix     nodiscord   the Discord app is not running
//   presence    Rich Presence on     notify      reply notifications on
//   cancel      sign-in: cancelled on Discord's page              expire   sign-in: the code runs out
//   deny        QR sign-in: declined on the phone                 tester   already a tester (with a test plan)
//   sent        tester: results sent, not read yet                read     tester: results read by the team
//   hidden      Settings' "Show the Community page" is off        noplan   tester, no test build open
//   nobuild     no test build open (a volunteer gets no checklist)
//   discordrefused  Discord refuses reports and results (502)     discordlimit  Discord's rate limit holds them (503)
// Tests and the console: __kilnMock.community.reply(), .approve(), .setOnline(bool), .setRelay(bool), .readResults(),
// .setDiscord("refused" | "limited" | null),
// .requests() (every call that would reach the relay, in order: what the "nothing is sent" tests check), .signins().
import type {
  CommunityAccount, CommunityFeed, CommunitySettings, CommunityStatus, Hardware, KnownProblem, ReportDraft, ReportPreview, SendFrom, SignInStart,
  SignInState, StepResult, SupportPost, TesterStatus, TestPlan, TestStep,
} from "./commands";

import { mockReportContents } from "./mockReport";

type Handler = (args: Record<string, unknown>) => unknown;
type Emit = (name: string, payload: unknown) => void;
export interface CommunityMockDeps {
  emit: Emit;
  /** Whether the fake game runs (Rich Presence shows only then). */
  gameRunning: () => boolean;
}

const q = typeof location !== "undefined" ? new URLSearchParams(location.search) : new URLSearchParams();
const FLAGS = new Set((q.get("discord") ?? "").split(",").map((s) => s.trim()).filter(Boolean));
const DECK = !!q.get("deck");
/** `&problem=crash|nokiln` (mock.ts plays it): the report the launcher prepares then is about it. */
const PROBLEM = q.get("problem");
const has = (f: string) => FLAGS.has(f);

const GUILD = "1568454645683195976";
const min = (n: number) => new Date(Date.now() - n * 60_000).toISOString();
const later = (n: number) => new Date(Date.now() + n * 60_000).toISOString();

const TESTER = has("tester") || has("sent") || has("read") || has("noplan");
/** The relay serves no test plan: no test build is open. */
const NO_BUILD = has("noplan") || has("nobuild");
const SIGNED_IN = has("in") || has("empty") || has("notmember") || TESTER;
const me = (): CommunityAccount => ({ id: "731204485922648124", name: "emberfox", member: !has("notmember"), tester });

let settings: CommunitySettings = { format: 1, shown: !has("hidden"), presence: has("presence"), notify: has("notify") };
let tester = TESTER;
let account: CommunityAccount | null = SIGNED_IN ? me() : null;
let online = !has("offline");
let relayUp = !has("relaydown");
/** Discord refuses the next report or results (the relay's 502 discord-refused), or its rate limit holds them (503
 *  discord-rate-limited), as kiln_manage::community::relay::refusal words them. */
let discord: "refused" | "limited" | null = has("discordrefused") ? "refused" : has("discordlimit") ? "limited" : null;
function discordAnswer() {
  if (discord === "refused") err("refused", "Discord didn't accept this from Kiln support.", "discord", { limited: false, status: 403, code: 50013, reason: "Missing Permissions" });
  if (discord === "limited") err("busy", "Discord asks Kiln support to wait before posting again.", "discord", { limited: true, waitMin: 2 });
}
/** The relay's sending limit the next report meets (docs/relay-api.md: hour, day, network), or none. */
let rateLimited: "hour" | "day" | "network" | null = has("ratelimit") ? "hour" : has("ratelimitday") ? "day" : has("ratelimitnet") ? "network" : null;
let postsAsOf: string | null = SIGNED_IN ? min(has("offline") ? 38 : 0) : null;
let mods: number | null = null;
/** Every call that would reach the relay (tests: `__kilnMock.community.requests()`). */
const requests: string[] = [];
/** Each sign-in's start as the launcher asks the relay (tests: `__kilnMock.community.signins()`). */
const signins: { mode: string; join: boolean }[] = [];

const thread = (n: number) => `https://discord.com/channels/${GUILD}/15731${n}0884210000${n}`;
let posts: SupportPost[] = !SIGNED_IN || has("empty") ? [] : [
  { id: "p3", title: "Mods don't load after the game update", url: thread(3), created: min(130), status: "replied", replies: 3, unread: 1,
    lastReply: { name: "Rowan", team: true, at: min(25) }, tags: ["Mods", "Linux"], zip: "team" },
  { id: "p2", title: "The launcher window is black at start", url: thread(2), created: min(60 * 26), status: "waiting", replies: 0, unread: 0,
    lastReply: null, tags: ["Launcher", "Linux"], zip: "team" },
  { id: "p1", title: "The game crashes when I open the map", url: thread(1), created: min(60 * 24 * 5), status: "solved", replies: 5, unread: 0,
    lastReply: { name: "Rowan", team: true, at: min(60 * 24 * 4) }, tags: ["Crash", "Linux", "Solved"], zip: "public" },
];

function err(code: string, message: string, kind: string, extra: Record<string, unknown> = {}): never {
  throw { code, message, detail: { kind, ...extra } };
}
/** A call that reaches the relay: refused while hidden, fails offline or with the relay down; logged. */
function relay(name: string) {
  if (!settings.shown) err("refused", "The Community page is turned off in Settings.", "hidden");
  requests.push(name);
  if (!online) err("network", "You're offline.", "offline");
  if (!relayUp) err("network", "Kiln support isn't answering right now.", "relay");
}
function needSession() {
  if (!account) err("auth_failed", "Sign in with Discord first.", "signin");
}

// ---------------------------------------------------------------- sign-in

interface Pending { id: string; code: string; mode: "browser" | "qr"; at: number; done: SignInState["state"] | null }
const pending = new Map<string, Pending>();
let seq = 0;
const CODES = ["KQ7M-4P2X", "HV3R-9TCW", "ZP4N-7KDB"];

// ---------------------------------------------------------------- reports

const KNOWN_NOKILN: KnownProblem = {
  id: "proton-override", title: "Steam's launch option is missing",
  fix: "Your Proton build needs one line in Enshrouded's launch options before Kiln can load. Copy it, paste it in Steam, and play again.",
  copy: 'WINEDLLOVERRIDES="dinput8=n,b" %command%',
};
const KNOWN_CRASH: KnownProblem = {
  id: "night-sky-weather-plus", title: "Night Sky and Weather Plus crash together",
  fix: "Night Sky 0.3 crashes the game when Weather Plus 1.4 changes the sky. Turn one of them off until Night Sky 0.4 is out.",
  action: "mods",
};

let preparedSeq = 0;
const prepared = new Map<string, { from: SendFrom; tags: string[] }>();

function preview(from: SendFrom, exclude?: unknown): ReportPreview {
  const contents = mockReportContents(exclude);
  const crash = from === "crash" || PROBLEM === "crash" || has("crash");
  const noKiln = from === "banner" || PROBLEM === "nokiln" || has("nokiln");
  const kindTags = { noload: "Mods", mod: "Mods", crash: "Crash", launcher: "Launcher", server: "Server", other: null };
  const tags = DECK ? ["Steam Deck", "Linux"] : ["Linux"];
  const id = `r${++preparedSeq}`;
  prepared.set(id, { from, tags });
  return {
    prepared: id,
    findings: noKiln ? [
      { id: "not-loaded", level: "problem", text: "The game ran, but Kiln did not load into it. Steam's launch options have no dinput8 override." },
      { id: "proton-override", level: "warning", text: "Kiln is in a Proton game and Steam's launch options have no dinput8 override." },
    ] : [
      { id: "mod-failed", level: "problem", text: "Night Sky 0.3.1 did not load: incompatible with Weather Plus 1.4.2." },
      { id: "crash", level: "warning", text: "Kiln wrote a crash report on 2026-11-18 at 22:10 (1 in all; the newest is in this report)." },
    ],
    // the relay's versions line (relay/post.mjs): a word and a value, or the value alone
    versions: [
      ["Kiln", "0.2.0-dev.29.18"], ["Launcher", "0.2.0-dev.29.18"], ["", DECK ? "SteamOS 3.7" : "Arch Linux"],
      ["", DECK ? "Proton 10.0" : "GE-Proton10-3"], ["game build", "26404966"], ["", `${mods ?? 12} mods on`],
    ],
    tags,
    kindTags,
    size: has("toobig") ? 21_400_000 : Math.round(contents.size / 2.2),
    limit: 8 * 1024 * 1024,
    files: contents.files.filter((f) => !f.excluded).map((f) => ({ path: f.path, size: f.size })),
    contents,
    known: noKiln ? KNOWN_NOKILN : crash || has("known") ? KNOWN_CRASH : null,
  };
}

// ---------------------------------------------------------------- testers

const hardware = (): Hardware => DECK
  ? { gpu: "AMD Custom GPU 0405 (Steam Deck LCD)", driver: "amdgpu (Mesa RADV)", vram: "1 GB shared", os: "SteamOS 3.7", runner: "Proton 10.0", caps: ["deck"], covers: ["Steam Deck"] }
  : { gpu: "AMD Radeon RX 7800 XT", driver: "amdgpu (Mesa RADV)", vram: "16 GB", os: "Arch Linux", runner: "GE-Proton10-3", caps: ["fsr4"], covers: ["FSR 4"] };

let beta = tester;
const after = has("sent") || has("read");
const planSteps = (): TestStep[] => [
  { id: "start", title: "Start the game with Kiln", for: null, how: ["Press Play.", "Load a world you play in.", "Play for five minutes."], result: after ? "worked" : null, note: "" },
  DECK
    ? { id: "deck-battery", title: "Play on battery for ten minutes", for: { label: "Steam Deck", needs: ["deck"] }, how: ["Unplug the Deck.", "Play for ten minutes in a busy base.", "Note if the game stutters or the fans get loud."],
        result: after ? "failed" : null, note: after ? "Stutters every few seconds near the forge" : "" }
    : { id: "fsr4", title: "Turn on FSR 4", for: { label: "AMD RX 6000 and newer", needs: ["fsr4"] }, how: ["Open Kiln's overlay in the game and choose Graphics.", "Set Upscaler to FSR 4.", "Walk around a base for two minutes.", "Look at leaves and water: do they shimmer or smear?"],
        result: after ? "failed" : null, note: after ? "Water smears when I turn the camera" : "" },
  { id: "overlay", title: "Open and close Kiln's overlay", for: null, how: ["Open Kiln's overlay.", "Close it again.", "Do this three times while you play."], result: after ? "worked" : null, note: "" },
  { id: "reload", title: "Quit to the menu and load again", for: null, how: ["Quit to the main menu.", "Load the same world again.", "Check that your mods still work."], result: after ? "skipped" : null, note: after ? "No time today" : "" },
];
const plan = (): TestPlan => ({
  build: "0.2.0-dev.30 Beta 2",
  url: `https://discord.com/channels/${GUILD}/1573990884210000077`,
  until: later(60 * 24 * 3),
  steps: planSteps(),
  hidden: 2,
  sent: has("read") ? { at: min(60 * 5), read: { by: "Rowan", at: min(60 * 2) }, replied: true } : has("sent") ? { at: min(20), read: null, replied: false } : null,
});
let testPlan: TestPlan | null = tester && !NO_BUILD ? plan() : null;

export function communityMockHandlers({ emit, gameRunning }: CommunityMockDeps): Record<string, Handler> {
  const changed = () => emit("kiln://community", { kind: "changed" });
  const status = (): CommunityStatus => ({
    shown: settings.shown,
    account: settings.shown ? account : null,
    online,
    relay: relayUp ? "ok" : "down",
    presence: { on: settings.presence, discordRunning: settings.presence && !has("nodiscord"), showing: settings.presence && !has("nodiscord") && gameRunning() },
    notify: settings.notify,
    unread: settings.shown && account ? posts.reduce((n, p) => n + p.unread, 0) : 0,
    postsAsOf,
  });
  return {
    community_settings_get: () => ({ ...settings }),
    community_settings_set: (a) => {
      const p = (a.patch ?? {}) as Partial<CommunitySettings>;
      settings = { ...settings, ...(p.shown !== undefined ? { shown: !!p.shown } : {}), ...(p.presence !== undefined ? { presence: !!p.presence } : {}), ...(p.notify !== undefined ? { notify: !!p.notify } : {}) };
      // hidden: a sign-in on its way ends (the backend closes its listener)
      if (!settings.shown) pending.clear();
      changed();
      return { ...settings };
    },
    community_status: () => status(),
    community_feed: (): CommunityFeed => {
      relay("feed");
      requests.push("known-issues");
      return {
        releases: [
          { title: "Kiln 0.2.0-dev.29.18", date: min(60 * 26), line: "Notes for OptiScaler on Play, and the Steam setting for GE-Proton 11-11 and newer.", url: "https://frostdev.io/kiln/download" },
          { title: "Kiln 0.2.0-dev.29.17", date: min(60 * 50), line: "Start the game from any folder, without Steam.", url: "https://frostdev.io/kiln/download" },
          { title: "Kiln 0.2.0-dev.29.16", date: min(60 * 75), line: "Fixes for mods that load late and for the save editor's amounts.", url: "https://frostdev.io/kiln/download" },
        ],
        events: [
          { title: "Community night: show your bases", start: "2026-11-21T20:00:00Z", where: "Voice channel, Kiln Discord", url: "https://discord.com/events/1568454645683195976/1" },
          { title: "Test weekend for dev.30", start: "2026-11-28T12:00:00Z", end: "2026-11-29T20:00:00Z", where: "#testing", url: "https://discord.com/events/1568454645683195976/2" },
        ],
      };
    },
    community_posts: (): SupportPost[] => {
      if (!account || !settings.shown) return [];
      relay("posts");
      postsAsOf = new Date().toISOString();
      return posts;
    },
    community_post_seen: (a) => { needSession(); relay("post-seen"); posts = posts.map((p) => (p.id === a.id ? { ...p, unread: 0 } : p)); changed(); return null; },
    community_post_solved: (a) => { needSession(); relay("post-solved"); posts = posts.map((p) => (p.id === a.id ? { ...p, status: "solved", unread: 0, tags: [...p.tags, "Solved"] } : p)); changed(); return null; },
    community_post_delete: (a) => { needSession(); relay("post-delete"); posts = posts.filter((p) => p.id !== a.id); changed(); return null; },
    community_signin_start: (a): SignInStart => {
      relay("signin");
      if (has("signinlimit")) err("busy", "Kiln support asks you to wait before sending again.", "rate_limited", { limit: "network", sent: 10, waitMin: 42, last: null });
      const id = `s${++seq}`;
      const code = CODES[(seq - 1) % CODES.length];
      const mode = a.mode === "qr" ? "qr" : "browser";
      signins.push({ mode, join: a.join === true });
      pending.set(id, { id, code, mode, at: Date.now(), done: null });
      return { id, code: mode === "qr" ? code : "", url: mode === "qr" ? `https://frostdev.io/link/${code.replace("-", "")}` : `https://frostdev.io/kiln/signin/${id}`, expiresAt: later(10) };
    },
    community_signin_poll: (a): SignInState => {
      const p = pending.get(String(a.id));
      if (!p) return { state: "expired" };
      if (p.done && p.done !== "approved") return { state: p.done };
      const due = p.mode === "qr" ? 9000 : 3500;
      if (p.done !== "approved" && Date.now() - p.at < due) return { state: "waiting" };
      if (p.done !== "approved") {
        if (has("cancel") && p.mode === "browser") { p.done = "canceled"; return { state: "canceled" }; }
        if (has("deny") && p.mode === "qr") { p.done = "denied"; return { state: "denied" }; }
        if (has("expire")) { p.done = "expired"; return { state: "expired" }; }
      }
      requests.push("signin-poll");
      pending.delete(p.id);
      account = me();
      changed();
      return { state: "approved", account };
    },
    community_signin_cancel: (a) => { pending.delete(String(a.id)); return null; },
    community_signout: (a) => {
      const told = !!account && settings.shown;
      if (told) requests.push(a.forget ? (a.posts ? "forget+posts" : "forget") : "signout");
      // as the backend does: the key goes here whatever frostdev.io answers; a forget it did not hear is the answer
      const heard = told && online && relayUp;
      account = null;
      postsAsOf = null;
      changed();
      if (a.forget && told && !heard) err("network", online ? "Kiln support isn't answering right now." : "You're offline.", online ? "relay" : "offline");
      if (a.forget) { tester = false; testPlan = null; }
      if (a.forget && a.posts) posts = [];
      return null;
    },
    community_check_member: () => {
      if (!account) return null;
      relay("me");
      if (has("notmember") && !joined) return account;
      account = { ...account, member: true };
      changed();
      return account;
    },
    community_steam_keyboard: () => null,
    community_presence_facts: (a) => { mods = typeof a.mods === "number" ? a.mods : null; return null; },
    community_report_prepare: (a) => preview((a.from as SendFrom) ?? "community", a.exclude),
    community_report_send: (a) => {
      const d = a.draft as ReportDraft;
      const p = prepared.get(String(a.prepared));
      if (!p) err("not_found", "This report was not prepared.", "not_found");
      if ((d.title?.trim().length ?? 0) < 3) err("invalid_argument", "Write one line first.", "title");
      needSession();
      relay("reports");
      if (!account!.member) err("refused", "You're not in the Kiln Discord yet.", "not_member");
      if (has("toonew")) err("refused", "This Discord account is too new to send reports.", "too_young");
      // the relay's 429 as kiln_manage::community::relay::refusal words it
      if (rateLimited) err("busy", "Kiln support asks you to wait before sending again.", "rate_limited", { limit: rateLimited, sent: { hour: 3, day: 10, network: 20 }[rateLimited],
        waitMin: rateLimited === "hour" ? 38 : 300, last: posts[0]?.url ?? null });
      if (has("toobig") && d.zip !== "none") err("refused", "The report is too big to send.", "too_big", { size: 21_400_000, limit: 8 * 1024 * 1024 });
      discordAnswer();
      const post: SupportPost = { id: `p${Date.now()}`, title: d.title, url: thread(9), created: new Date().toISOString(), status: "waiting", replies: 0, unread: 0,
        lastReply: null, tags: [...new Set([({ noload: "Mods", mod: "Mods", crash: "Crash", launcher: "Launcher", server: "Server", other: null } as Record<string, string | null>)[d.kind], ...p!.tags].filter((x): x is string => !!x))], zip: d.zip };
      posts = [post, ...posts];
      prepared.delete(String(a.prepared));
      changed();
      return post;
    },
    // testers
    tester_status: (): TesterStatus => {
      if (account && settings.shown && online && relayUp) requests.push("tester");
      return { tester: !!account && tester, beta, hardware: hardware(), plan: account && tester ? testPlan : null };
    },
    tester_join: (a) => {
      needSession();
      relay("tester-join");
      tester = true;
      if (a.beta) beta = true;
      account = { ...account!, tester: true };
      testPlan = NO_BUILD ? null : { ...plan(), sent: null };
      if (testPlan) testPlan.steps = testPlan.steps.map((s) => ({ ...s, result: null, note: "" }));
      changed();
      return null;
    },
    tester_leave: () => { needSession(); relay("tester-leave"); tester = false; testPlan = null; if (account) account = { ...account, tester: false }; changed(); return null; },
    tester_result_set: (a) => {
      if (!testPlan) return null;
      if (testPlan.sent) err("refused", "These results were sent already.", "sent");
      testPlan = { ...testPlan, steps: testPlan.steps.map((s) => (s.id === a.step ? {
        ...s,
        result: a.result === undefined ? s.result : a.result === "clear" ? null : (a.result as StepResult),
        note: a.note === undefined ? s.note : String(a.note).slice(0, 140),
      } : s)) };
      return testPlan;
    },
    tester_results_send: (a) => {
      needSession();
      if (!testPlan) return null;
      // the attached report goes only as prepared and shown (the backend refuses one it did not show)
      if (a.withReport && !prepared.has(String(a.prepared))) err("invalid_argument", "The report to attach was not shown yet.", "not_found");
      if (testPlan.steps.some((s) => !s.result)) err("invalid_argument", "Try each step, or mark it Couldn't try.", "untried");
      relay(a.withReport ? "tester-results+zip" : "tester-results");
      discordAnswer();
      // used up only once sent (as community/testers.rs send_results does): a failed send can be tried again
      if (a.withReport) prepared.delete(String(a.prepared));
      testPlan = { ...testPlan, sent: { at: new Date().toISOString(), read: null, replied: false } };
      changed();
      return testPlan;
    },
  };
}

let joined = false;

/** Console and test hooks (window.__kilnMock.community). */
export function communityMockHooks(emit: Emit) {
  return {
    /** Kiln support replies to the newest open post. */
    reply: (name = "Rowan") => {
      const p = posts.find((x) => x.status !== "solved");
      if (!p) return;
      posts = posts.map((x) => (x === p ? { ...x, status: "replied" as const, replies: x.replies + 1, unread: x.unread + 1, lastReply: { name, team: true, at: new Date().toISOString() } } : x));
      emit("kiln://community", { kind: "reply", post: p.id, title: p.title, url: p.url, by: name });
    },
    approve: () => { for (const p of pending.values()) p.done = "approved"; },
    setOnline: (on: boolean) => { online = on; emit("kiln://community", { kind: "changed" }); },
    setRelay: (up: boolean) => { relayUp = up; emit("kiln://community", { kind: "changed" }); },
    setDiscord: (mode: "refused" | "limited" | null) => { discord = mode; },
    setRateLimited: (on: boolean | "hour" | "day" | "network") => { rateLimited = on === true ? "hour" : on || null; },
    /** "I've joined": the next membership check finds the player in the server. */
    join: () => { joined = true; },
    readResults: () => { if (testPlan?.sent) testPlan = { ...testPlan, sent: { ...testPlan.sent, read: { by: "Rowan", at: new Date().toISOString() } } }; emit("kiln://community", { kind: "changed" }); },
    requests: () => [...requests],
    /** The prepared reports run out (the backend keeps one 30 minutes). */
    forgetPrepared: () => prepared.clear(),
    signins: () => signins.map((x) => ({ ...x })),
  };
}
