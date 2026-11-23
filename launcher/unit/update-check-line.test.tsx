// components/UpdateSettings.tsx: what About, Settings and the Deck say about the launcher's own update check. After a
// check: Kiln is up to date with its version and the local time of the check, the version found, or why the check
// failed (each source's answer, or the error of the call); while it checks, that it does.
import { describe, expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import type { UpdateStatus } from "../src/api/commands";
import { CheckLineText, checkLine, checkedTime, checkFailure } from "../src/components/UpdateSettings";

const at = "2026-11-23T17:42:00Z";
const now = new Date(at);
const status = (p: Partial<UpdateStatus> = {}): UpdateStatus => ({
  current: "0.2.0-dev.42", distribution: "direct", channel: "dev", auto: true, install: { kind: "appimage", inPlace: true, reason: null },
  page: "https://frostdev.io/kiln", phase: "upToDate", checked: at, available: null, failures: [], progress: null, ready: null, skipped: null,
  justUpdated: null, taskId: null, error: null, ...p,
});
const clock = (iso: string) => new Date(iso).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
const failures = [
  { source: "frostdev.io", step: "manifest" as const, message: "not reachable (timed out)" },
  { source: "GitHub", step: "manifest" as const, message: "not reachable (dns error: no such host)" },
];

describe("the line after a check", () => {
  test("already current: Kiln is up to date, its version, and the local time of the check", () => {
    expect(checkLine(status(), { now })).toEqual({ text: `Kiln is up to date (0.2.0-dev.42) · checked ${clock(at)}`, failed: false });
  });
  test("a test build names its beta number", () => {
    expect(checkLine(status({ current: "0.2.0-dev.43", beta: 3 }), { now }).text).toBe(`Kiln is up to date (0.2.0-dev.43 beta 3) · checked ${clock(at)}`);
  });
  test("a check on another day says the day too", () => {
    const before = "2026-11-21T12:00:00Z";
    expect(checkedTime(before, now)).toBe(new Date(before).toLocaleString(undefined, { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" }));
    expect(checkedTime(before, now)).not.toBe(clock(before));
    expect(checkLine(status({ checked: before }), { now }).text).toBe(`Kiln is up to date (0.2.0-dev.42) · checked ${checkedTime(before, now)}`);
  });
  test("while it checks it says so, whether this window asked or the backend already checks", () => {
    expect(checkLine(status(), { busy: true, now }).text).toBe("Checking for updates…");
    expect(checkLine(status({ phase: "checking" }), { now }).text).toBe("Checking for updates…");
  });
  test("an update found: its version and when", () => {
    const available = { version: "0.2.0-dev.43", notes: null, date: null, source: "frostdev.io", page: "https://frostdev.io/kiln", runtime: null };
    expect(checkLine(status({ phase: "available", available }), { now }).text).toBe(`Kiln 0.2.0-dev.43 is available · checked ${clock(at)}`);
  });
  test("a failed check says why: each source's answer, then when", () => {
    expect(checkLine(status({ phase: "failed", failures }), { now })).toEqual({
      text: `Check failed: frostdev.io: not reachable (timed out); GitHub: not reachable (dns error: no such host) · checked ${clock(at)}`, failed: true,
    });
    expect(checkFailure(status({ phase: "failed", failures }))).toBe("frostdev.io: not reachable (timed out); GitHub: not reachable (dns error: no such host)");
  });
  test("a check the launcher could not even ask: the call's error, on the line", () => {
    expect(checkLine(status(), { error: "IPC unavailable", now })).toEqual({ text: "Check failed: IPC unavailable", failed: true });
    expect(checkFailure(status({ phase: "failed", error: { code: "io", message: "the settings did not read" } }))).toBe("the settings did not read");
    expect(checkFailure(status({ phase: "failed" }))).toBe("no update server answered");
  });
  test("never checked, or checks off: the short words as before", () => {
    expect(checkLine(status({ phase: "idle", checked: null }), { now }).text).toBe("Not checked yet");
    expect(checkLine(status({ phase: "idle", checked: null, auto: false }), { now }).text).toBe("Off");
  });
});

test("a failed check shows as an error on the line itself", () => {
  const failed = renderToStaticMarkup(<CheckLineText s={status({ phase: "failed", failures })} />);
  expect(failed).toContain('class="k-error-text"');
  expect(failed).toContain("Check failed: frostdev.io: not reachable (timed out)");
  expect(renderToStaticMarkup(<CheckLineText s={status()} />)).not.toContain("k-error-text");
});
