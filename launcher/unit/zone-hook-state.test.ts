import { expect, test } from "bun:test";
import { zoneHookNotices } from "../src/admin/zoneHookState";

test("zone notices distinguish unused, waiting and refused group rows", () => {
  expect(zoneHookNotices({ available: true, intercept: true, hookGroups: {
    build: { state: "idle", reason: "resolved, not installed: no mod asked" },
    terraform: { state: "waiting", reason: "safe Pre" },
    spawn: { state: "refused", reason: "thread check" },
  } })).toEqual(["Building and breaking: not in use yet.", "Digging: waiting: safe Pre.", "Enemy spawns: refused: thread check."]);
});
test("a disabled craft observer never implies craft protection is off", () => {
  expect(zoneHookNotices({ available: true, intercept: true, craftHook: false, buildHooks: true, terraformHook: true, spawnHook: true })).toEqual([]);
});
test("old servers with a false observer flag have unknown installation status", () => {
  expect(zoneHookNotices({ available: true, buildHooks: false, terraformHook: true, spawnHook: true })[0]).toContain("status unknown");
});
test("installed rows override stale flags and craft protection follows interception", () => {
  expect(zoneHookNotices({ available: true, intercept: false, hookGroups: {
    build: { state: "installed", reason: "" }, terraform: { state: "installed", reason: "" }, spawn: { state: "installed", reason: "" },
  } })).toEqual(["Opening, looting and crafting: Pre interception unavailable."]);
});
