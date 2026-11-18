// The Discord features' logic in the window (src/community/state.ts, parts.tsx) and the mock's rules that stand in for
// the backend's (src/api/mockCommunity.ts): the page's sentence, a post's state word, the hide setting, and that
// nothing reaches the relay while hidden or before the page asks.
import { expect, test } from "bun:test";
import * as api from "../src/api/commands";
import type { CommunityStatus } from "../src/api/commands";
import { ago, errKind, sayLine } from "../src/community/state";
import { statusWord } from "../src/community/parts";

const st = (p: Partial<CommunityStatus>): CommunityStatus => ({
  shown: true, account: null, online: true, relay: "ok", presence: { on: false, discordRunning: false, showing: false }, notify: false, unread: 0, postsAsOf: null, ...p,
});
const me = { id: "1", name: "emberfox", member: true, tester: false };

test("the page's sentence follows the account, the replies and the network", () => {
  expect(sayLine(st({}))).toBe("Kiln's Discord: help from the Kiln team and from other players. All of it is optional.");
  expect(sayLine(st({}), true)).toBe("Help from the Kiln team on Discord. Optional.");
  expect(sayLine(st({ account: me }))).toBe("Signed in as emberfox.");
  expect(sayLine(st({ account: me, unread: 2 }))).toBe("Signed in as emberfox. 2 new replies from Kiln support.");
  expect(sayLine(st({ account: me, unread: 1 }))).toBe("Signed in as emberfox. 1 new reply from Kiln support.");
  expect(sayLine(st({ online: false }))).toBe("You're offline. This page needs the internet.");
  expect(sayLine(st({ account: me, online: false, postsAsOf: "2026-11-18T11:47:00Z" }))).toMatch(/^You're offline\. Your posts show as they were at .+\.$/);
  expect(sayLine(st({ account: me, relay: "down", postsAsOf: "2026-11-18T11:47:00Z" }))).toMatch(/^Kiln support isn't answering right now\./);
  expect(sayLine(undefined)).toBe("");
});

test("a post's word: Waiting, New reply while unread, Replied, Solved, Closed", () => {
  expect(statusWord({ status: "waiting", unread: 0 })).toEqual({ tone: "idle", key: "waiting" });
  expect(statusWord({ status: "replied", unread: 1 })).toEqual({ tone: "ember", key: "replied" });
  expect(statusWord({ status: "replied", unread: 0 })).toEqual({ tone: "info", key: "repliedSeen" });
  expect(statusWord({ status: "solved", unread: 0 })).toEqual({ tone: "ok", key: "solved" });
  expect(statusWord({ status: "closed", unread: 0 })).toEqual({ tone: "idle", key: "closed" });
});

test("ago is said roughly, as the rest of the launcher says it", () => {
  const now = Date.parse("2026-11-18T12:00:00Z");
  expect(ago("2026-11-18T11:59:30Z", now)).toBe("just now");
  expect(ago("2026-11-18T11:35:00Z", now)).toBe("25 minutes ago");
  expect(ago("2026-11-18T11:00:00Z", now)).toBe("an hour ago");
  expect(ago("2026-11-17T10:00:00Z", now)).toBe("26 hours ago");
  expect(ago("2026-11-16T12:00:00Z", now)).toBe("2 days ago");
});

test("signed out, nothing but the public news reaches the relay; hidden, not even that", async () => {
  const { communityMockHooks } = await import("../src/api/mockCommunity");
  const requests = () => communityMockHooks(() => {}).requests();
  // the status, the settings and the tester's hardware are local
  await api.communityStatus();
  await api.communitySettingsGet();
  await api.testerStatus();
  expect(await api.communityPosts()).toEqual([]);
  expect(requests()).toEqual([]);
  await api.communityFeed();
  expect(requests()).toEqual(["feed", "known-issues"]);
  // Settings' "Show the Community page" off: every call that would reach the relay is refused before it is made
  await api.communitySettingsSet({ shown: false });
  const before = requests().length;
  for (const call of [() => api.communityFeed(), () => api.communitySignInStart("browser", true)]) {
    const e = await call().then(() => null, (x) => x);
    expect(errKind(e)).toBe("hidden");
  }
  expect((await api.communityStatus()).account).toBeNull();
  expect(requests().length).toBe(before);
  await api.communitySettingsSet({ shown: true });
});
