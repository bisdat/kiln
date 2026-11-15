import { test, expect } from "bun:test";
import type { Profile, ShroudlinkNotice } from "../src/api/commands";
import { shroudlinkNoticeText, SHROUDLINK_NOTICE, SHROUDLINK_DOWNLOAD } from "../src/shell/shroudlinkNotice";
const p = { id: "test", created: "now", mods: { shroudlink: [{ enabled: true }] } } as Profile;
const n: ShroudlinkNotice = { mods: ["assistants"], addons: [{ kind: "native", id: "addon", version: "1", hash: "a" }] };
test("one wording and existing-addon action text", () => expect(shroudlinkNoticeText({ kind: "client" }, p.id, p, n)).toBe(SHROUDLINK_NOTICE));
test("absent Library addon names the download", () => expect(shroudlinkNoticeText({ kind: "client" }, p.id, p, { ...n, addons: [] })).toBe(`${SHROUDLINK_NOTICE} ${SHROUDLINK_DOWNLOAD}`));
test("server and no-client installs stay silent", () => {
  expect(shroudlinkNoticeText({ kind: "server" }, p.id, p, n)).toBeNull();
  expect(shroudlinkNoticeText(null, p.id, p, n)).toBeNull();
});
test("no enabled Shroudlink mods stays silent", () => {
  for (const shroudlink of [[], [{ enabled: false }]]) expect(shroudlinkNoticeText({ kind: "client" }, p.id, { ...p, mods: { shroudlink } } as Profile, n)).toBeNull();
});
test("loaded addon, pending response and profile switch stay silent", () => {
  expect(shroudlinkNoticeText({ kind: "client" }, p.id, p, null)).toBeNull();
  expect(shroudlinkNoticeText({ kind: "client" }, p.id, p, undefined)).toBeNull();
  expect(shroudlinkNoticeText({ kind: "client" }, "other", p, n)).toBeNull();
  expect(shroudlinkNoticeText({ kind: "client" }, null, p, n)).toBeNull();
});
