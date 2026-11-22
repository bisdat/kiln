// SPDX-License-Identifier: MIT
// The debugger's safety rules, headless: DAP framing, the guard's policy, the target from `kiln debug --json`, and dist/dapProxy.js as a
// real process around the mock netcoredbg (test/fixtures/dap), including every way a session can end.
import * as assert from "node:assert/strict";
import { ChildProcess, spawn, spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { DapGuard, DapMessage, DapReader, encodeDap, ProxySpec, staleContextsWarning } from "../../src/core/dap";
import {
  activeDebugTargets, adapterKind, attachedSession, DebugRefused, debugArgs, describeDebugState, describeResolution, findNetcoredbg, netcoredbgCandidates,
  netcoredbgEnv, parseResolution, proxySpecFromCli, refusalMessage, reloadBlockedBy, resolveWithCli, selectTarget, unloadsModCode,
} from "../../src/core/debugTarget";

const EXT = path.resolve(__dirname, "../../..");
const PROXY = path.join(EXT, "dist", "dapProxy.js");
const MOCK = path.join(EXT, "test", "fixtures", "dap", "mock-netcoredbg.js");
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "kiln-vscode-dap-"));
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

suite("DAP framing", () => {
  test("frames split anywhere, several in a chunk, multi-byte bodies", () => {
    const got: DapMessage[] = [];
    const r = new DapReader((m) => got.push(m));
    const a = encodeDap({ seq: 1, type: "event", event: "output", body: { output: "é🔥" } });
    const b = encodeDap({ seq: 2, type: "event", event: "stopped" });
    const all = Buffer.concat([a, b]);
    for (let i = 0; i < all.length; i += 7) r.push(all.subarray(i, i + 7));
    assert.equal(got.length, 2);
    assert.equal(got[0].body.output, "é🔥");
    assert.equal(got[1].event, "stopped");
  });
  test("a line that is not a frame is skipped and reported", () => {
    const got: DapMessage[] = [];
    const errs: Error[] = [];
    const r = new DapReader((m) => got.push(m), (e) => errs.push(e));
    r.push(Buffer.concat([Buffer.from("wine: some noise\r\n\r\n"), encodeDap({ seq: 3, type: "event", event: "x" })]));
    assert.equal(got.length, 1);
    assert.equal(errs.length, 1);
  });
});

suite("DAP guard", () => {
  function setup(pid: number | undefined = 4242) {
    const toAdapter: DapMessage[] = [];
    const toClient: DapMessage[] = [];
    return { g: new DapGuard({ toAdapter: (m) => toAdapter.push(m), toClient: (m) => toClient.push(m) }, pid), toAdapter, toClient };
  }
  const req = (seq: number, command: string, args?: object): DapMessage => ({ seq, type: "request", command, arguments: args });

  test("attach gets the server's pid when it is missing or 0", () => {
    for (const args of [undefined, {}, { processId: 0 }, { processId: 4242 }]) {
      const { g, toAdapter } = setup();
      g.fromClient(req(1, "attach", args));
      assert.equal(toAdapter[0].arguments.processId, 4242);
      assert.ok(g.mustDisconnect);
    }
  });
  test("attach to any other pid is an error, not rewritten", () => {
    const { g, toAdapter, toClient } = setup();
    g.fromClient(req(1, "attach", { processId: 99 }));
    assert.equal(toAdapter.length, 0);
    assert.equal(toClient[0].success, false);
    assert.match(toClient[0].message!, /4242/);
    assert.equal(g.mustDisconnect, false);
  });
  test("every disconnect keeps the debuggee", () => {
    for (const args of [undefined, {}, { terminateDebuggee: true }, { terminateDebuggee: true, restart: true, suspendDebuggee: true }]) {
      const { g, toAdapter } = setup();
      g.fromClient(req(1, "disconnect", args));
      assert.deepEqual(toAdapter[0].arguments, { terminateDebuggee: false });
    }
  });
  test("what could start or end a process never reaches the debugger", () => {
    for (const cmd of ["launch", "terminate", "restart", "terminateThreads"]) {
      const { g, toAdapter, toClient } = setup();
      g.fromClient(req(1, cmd, {}));
      assert.equal(toAdapter.length, 0, cmd);
      assert.equal(toClient[0].success, false, cmd);
      assert.equal(toClient[0].request_seq, 1);
    }
  });
  test("a request named like an Object.prototype member passes through", () => {
    const { g, toAdapter } = setup();
    g.fromClient(req(1, "constructor", {}));
    assert.equal(toAdapter.length, 1);
  });
  test("the initialize response stops advertising them", () => {
    const { g, toClient } = setup();
    g.fromAdapter({ seq: 1, type: "response", request_seq: 1, command: "initialize", success: true, body: { supportsTerminateRequest: true, supportTerminateDebuggee: true, supportsRestartRequest: true, supportsConfigurationDoneRequest: true } });
    const b = toClient[0].body;
    assert.deepEqual([b.supportsTerminateRequest, b.supportTerminateDebuggee, b.supportSuspendDebuggee, b.supportsRestartRequest, b.supportsTerminateThreadsRequest], [false, false, false, false, false]);
    assert.equal(b.supportsConfigurationDoneRequest, true);
  });
  test("our own disconnect: sent once, its response stays private, detached on success only", async () => {
    const { g, toAdapter, toClient } = setup();
    g.fromClient(req(1, "attach", {}));
    const p = g.disconnect();
    const q = g.disconnect();
    const own = toAdapter.filter((m) => m.command === "disconnect");
    assert.equal(own.length, 1);
    assert.equal(own[0].arguments.terminateDebuggee, false);
    g.fromAdapter({ seq: 2, type: "response", request_seq: own[0].seq, command: "disconnect", success: true });
    assert.deepEqual(await Promise.all([p, q]), [true, true]);
    assert.equal(g.mustDisconnect, false);
    assert.equal(toClient.length, 0);
  });
  test("a failed disconnect is not a detach; the client's own one is joined, not repeated", async () => {
    const { g, toAdapter, toClient } = setup();
    g.fromClient(req(1, "attach", {}));
    g.fromClient(req(2, "disconnect", {}));
    const p = g.disconnect();
    assert.equal(toAdapter.filter((m) => m.command === "disconnect").length, 1);
    g.fromAdapter({ seq: 3, type: "response", request_seq: 2, command: "disconnect", success: false, message: "no" });
    assert.equal(await p, false);
    assert.equal(g.mustDisconnect, true);
    assert.equal(toClient.length, 1);
  });
  test("a module that comes a second time is a reload: forwarded, and the client is told", () => {
    const { g, toClient } = setup();
    const mod = (name: string, reason = "new"): DapMessage => ({ seq: 1, type: "event", event: "module", body: { reason, module: { name } } });
    g.fromAdapter(mod("SampleTests"));
    g.fromAdapter(mod("SampleLib"));
    g.fromAdapter(mod("SampleTests", "removed"));
    assert.equal(toClient.length, 3);
    g.fromAdapter(mod("SampleTests"));
    assert.equal(toClient.length, 5);
    assert.equal(toClient[3].event, "module");
    assert.equal(toClient[4].body.category, "important");
    assert.match(toClient[4].body.output, /SampleTests was reloaded/);
  });
  test("the answer of gc: a warning only when old copies stay", () => {
    assert.equal(staleContextsWarning("0 unloaded mod context(s) still alive"), undefined);
    assert.match(staleContextsWarning("2 unloaded mod context(s) still alive")!, /Restart the server/);
    assert.equal(staleContextsWarning("collected"), undefined);
  });
  test("no debugger: every request gets the reason, disconnect succeeds", () => {
    const { g, toAdapter, toClient } = setup();
    g.failure = "no netcoredbg";
    g.fromClient(req(1, "initialize", {}));
    g.fromClient(req(2, "attach", {}));
    g.fromClient(req(3, "disconnect", {}));
    assert.equal(toAdapter.length, 0);
    assert.deepEqual(toClient.map((m) => [m.success, m.message]), [[false, "no netcoredbg"], [false, "no netcoredbg"], [true, undefined]]);
  });
  test("the debugger gone: open requests are answered, disconnect does not wait", async () => {
    const { g, toClient } = setup();
    g.fromClient(req(1, "attach", {}));
    g.adapterExited("it exited");
    assert.equal(toClient[0].success, false);
    assert.equal(toClient[0].request_seq, 1);
    assert.equal(g.mustDisconnect, false);
    assert.equal(await g.disconnect(), false);
  });
});

suite("debug target", () => {
  /** A stand-in `kiln`: logs its argv and answers `debug … --json` from a table keyed by the server folder or client. */
  function fakeCli(d: string, answers: Record<string, object>): { cli: string; calls: () => string[][] } {
    const log = path.join(d, "cli-calls.log");
    const cli = path.join(d, "kiln");
    fs.writeFileSync(
      cli,
      `#!${process.execPath}
const fs = require("fs"), a = process.argv.slice(2);
fs.appendFileSync(${JSON.stringify(log)}, JSON.stringify(a) + "\\n");
const answers = ${JSON.stringify(answers)};
const key = a[a.indexOf(a.includes("--server") ? "--server" : "--client") + 1];
const v = answers[key] ?? { ok: false, error: { code: "not_found", message: "no server or install " + key } };
console.log(JSON.stringify(v));
process.exit(v.ok ? 0 : 1);
`,
      { mode: 0o755 },
    );
    return { cli, calls: () => (fs.existsSync(log) ? fs.readFileSync(log, "utf8").split("\n").filter(Boolean).map((l) => JSON.parse(l)) : []) };
  }
  /** What `kiln debug --json` answers for a proven target (kiln_manage::debug's shape, REPORT-S1-S2 §6). */
  const resolution = (dir: string, windowsPid: number, more: object = {}) => ({
    ok: true, kind: "server", server: dir, install: dir, via: "folder", lab: null, pid: windowsPid, hostPid: 9000 + windowsPid,
    wine: "/r/bin/wine", prefix: `${dir}.pfx`, netcoredbg: "/t/netcoredbg.exe",
    env: { WINEPREFIX: `${dir}.pfx`, WINEDEBUG: "-all" }, command: ["/r/bin/wine", "/t/netcoredbg.exe", "--interpreter=vscode"], maxPauseMs: null,
    descriptor: path.join(dir, "kiln", "state", "debug", `${windowsPid}.json`), notes: [],
    target: {
      identity: { installId: "i", prefixIdentity: null, runnerSha256: null, windowsPid, creationTime100ns: "134390977514834040", imageSha256: "ab".repeat(32), sessionNonce: "cd".repeat(16) },
      image: "Z:\\s\\enshrouded_server.exe", runtimeVersion: "10.0.14", managedReady: true, sessionMode: "hosted", runnerContext: null, hostPid: 9000 + windowsPid,
    },
    ...more,
  });

  test("one target: server or client, never both; maxPauseMs 100..60000", () => {
    assert.deepEqual(selectTarget({}), { kind: "server", server: undefined });
    assert.deepEqual(selectTarget({ server: " lab " }), { kind: "server", server: "lab" });
    assert.deepEqual(selectTarget({ client: "steam-client", allowClientPause: true }), { kind: "client", client: "steam-client" });
    assert.throws(() => selectTarget({ server: "a", client: "b", allowClientPause: true }), (e: DebugRefused) => e.code === "usage");
    for (const bad of [99, 60_001, 1.5, Number.NaN]) assert.throws(() => selectTarget({ server: "a", maxPauseMs: bad }), (e: DebugRefused) => e.code === "usage", String(bad));
    assert.equal(selectTarget({ server: "a", maxPauseMs: 100 }).kind, "server");
  });

  test("a client configuration without allowClientPause shows the consent error, and nothing runs", () => {
    const d = tmp();
    const { calls } = fakeCli(d, {});
    for (const c of [{ client: "steam-client" }, { client: "steam-client", allowClientPause: false }, { client: "x", allowClientPause: "true" as unknown as boolean }]) {
      assert.throws(() => selectTarget(c), (e: DebugRefused) => {
        assert.equal(e.code, "pause_consent_required");
        assert.match(refusalMessage(e), /allowClientPause/);
        assert.match(refusalMessage(e), /--observe/);
        return true;
      });
    }
    assert.deepEqual(calls(), [], "the configuration is refused before the CLI runs");
  });

  test("arguments: the server's folder, or the client with its consent flag; maxPauseMs passed on", () => {
    assert.deepEqual(debugArgs({ kind: "server", dir: "/srv/a" }, {}), ["debug", "--server", "/srv/a"]);
    assert.deepEqual(debugArgs({ kind: "client", client: "steam-client" }, { maxPauseMs: 2000 }), ["debug", "--client", "steam-client", "--allow-client-pause", "--max-pause-ms", "2000"]);
  });

  test("two Windows servers resolve by their folders, an unknown or ambiguous one is refused", async () => {
    const d = tmp();
    const a = path.join(d, "A"), b = path.join(d, "B"), both = path.join(d, "both");
    const { cli, calls } = fakeCli(d, {
      [a]: resolution(a, 100),
      [b]: resolution(b, 200),
      [both]: { ok: false, error: { code: "refused", message: "2 descriptors match this server", detail: { reason: "several_descriptors" } } },
    });
    const ra = await resolveWithCli(cli, debugArgs({ kind: "server", dir: a }, {}), {});
    const rb = await resolveWithCli(cli, debugArgs({ kind: "server", dir: b }, {}), {});
    assert.equal(ra.pid, 100);
    assert.equal(rb.pid, 200);
    assert.equal(proxySpecFromCli(ra).pid, 100);
    await assert.rejects(resolveWithCli(cli, debugArgs({ kind: "server", dir: both }, {}), {}), (e: DebugRefused) => e.code === "refused" && e.reason === "several_descriptors");
    await assert.rejects(resolveWithCli(cli, debugArgs({ kind: "server", dir: path.join(d, "C") }, {}), {}), (e: DebugRefused) => e.code === "not_found");
    assert.deepEqual(calls().map((c) => c.slice(0, 3).concat(c.slice(-1))), [
      ["debug", "--server", a, "--json"], ["debug", "--server", b, "--json"], ["debug", "--server", both, "--json"], ["debug", "--server", path.join(d, "C"), "--json"],
    ]);
  });

  test("no code path runs tasklist or lists processes itself: only the kiln CLI runs", async () => {
    // the source: no tasklist, no wine, no process listing in the extension (comments aside)
    const files: string[] = [];
    const walk = (p: string) => { for (const f of fs.readdirSync(p, { withFileTypes: true })) f.isDirectory() ? walk(path.join(p, f.name)) : f.name.endsWith(".ts") && files.push(path.join(p, f.name)); };
    walk(path.join(EXT, "src"));
    for (const f of files) {
      const code = fs.readFileSync(f, "utf8").replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "").replace(/ \/\/ .*$/gm, "");
      assert.doesNotMatch(code, /tasklist/i, f);
    }
    // a run: tasklist and wine on PATH would leave a marker; the fake CLI is the only process
    const d = tmp();
    const bin = path.join(d, "bin");
    fs.mkdirSync(bin);
    const marker = path.join(d, "ran");
    for (const n of ["tasklist", "tasklist.exe", "wine", "wine64", "ps", "pgrep"]) fs.writeFileSync(path.join(bin, n), `#!/bin/sh\necho ${n} >> ${marker}\n`, { mode: 0o755 });
    const dir = path.join(d, "lab");
    const { cli, calls } = fakeCli(d, { [dir]: resolution(dir, 32) });
    const r = await resolveWithCli(cli, debugArgs({ kind: "server", dir }, {}), { PATH: `${bin}:${process.env.PATH}` });
    assert.equal(r.target.identity.windowsPid, 32);
    assert.equal(calls().length, 1);
    assert.equal(fs.existsSync(marker), false, "nothing but the CLI ran");
  });

  test("the CLI's answer: refusal codes and reasons kept, an unknown code is refused, an old CLI without identity is tool_missing", () => {
    assert.throws(() => parseResolution(JSON.stringify({ ok: false, error: { code: "pause_consent_required", message: "m" } })), (e: DebugRefused) => e.code === "pause_consent_required");
    assert.throws(() => parseResolution(JSON.stringify({ ok: false, error: { code: "identity_changed", message: "m", detail: { reason: "nonce" } } })), (e: DebugRefused) => e.code === "identity_changed" && e.reason === "nonce");
    assert.throws(() => parseResolution(JSON.stringify({ ok: false, error: { code: "new_code", message: "m" } })), (e: DebugRefused) => e.code === "refused");
    assert.throws(() => parseResolution(JSON.stringify({ ok: true, pid: 4242 })), (e: DebugRefused) => e.code === "tool_missing", "the pre-dev.31 shape (pid only)");
    assert.throws(() => parseResolution("not json"), (e: DebugRefused) => e.code === "tool_missing");
    const r = parseResolution(JSON.stringify(resolution("/s", 7)));
    assert.match(describeResolution(r), /Windows PID 7 \(created 134390977514834040\).*mode hosted.*host pid 9007 \(display only\)/);
  });

  test("the built-in proxy is only a transport: the CLI's own command, environment and PID", () => {
    const r = parseResolution(JSON.stringify(resolution("/s", 32)));
    const spec = proxySpecFromCli(r);
    assert.equal(spec.command, "/r/bin/wine");
    assert.deepEqual(spec.args, ["/t/netcoredbg.exe", "--interpreter=vscode"]);
    assert.deepEqual(spec.env, { WINEPREFIX: "/s.pfx", WINEDEBUG: "-all" });
    assert.equal(spec.pid, 32);
    assert.equal(spec.cwd, "/t");
  });

  test("kiln/debugState: the watchdog's continue and the end warn, a mode change informs, the rest is a log line", () => {
    const id = { windowsPid: 32 };
    assert.deepEqual(describeDebugState({ identity: id, state: "running", sessionMode: "hosted", reason: "timeout_continue", maxPauseMs: 5000 }), {
      text: "debug: running (timeout_continue), PID 32, mode hosted, pauses end after 5000 ms", notify: "warn",
    });
    assert.equal(describeDebugState({ identity: id, state: "ended", reason: "debuggee_exited" }).notify, "warn");
    assert.equal(describeDebugState({ identity: id, state: "paused", sessionMode: "joined", reason: "session_mode_changed" }).notify, "info");
    assert.equal(describeDebugState({ identity: id, state: "paused", sessionMode: "menu" }).notify, "none");
    assert.equal(describeDebugState(undefined).text, "debug: ?, PID ?, mode unknown");
  });

  test("refusal messages point at the fix", () => {
    assert.match(refusalMessage(new DebugRefused("refused", "developer debugging is off", "developer_debugging_off")), /Developer debugging/);
    assert.equal(refusalMessage(new DebugRefused("ambiguous_runner", "two runners", "runner")), "two runners [ambiguous_runner: runner]");
    assert.equal(refusalMessage(new DebugRefused("tool_missing", "no helper")), "no helper");
  });

  test("netcoredbg lookup: setting, $KILN_NETCOREDBG, data folder; an explicit path never falls through", () => {
    assert.deepEqual(netcoredbgCandidates("/a/n.exe", { KILN_NETCOREDBG: "/b/n.exe" }), ["/a/n.exe"]);
    assert.deepEqual(netcoredbgCandidates("", { KILN_NETCOREDBG: "/b/n.exe" }), ["/b/n.exe"]);
    assert.deepEqual(netcoredbgCandidates("", { KILN_DATA_DIR: "/d" }), [path.join("/d", "tools", "netcoredbg", "netcoredbg.exe")]);
    assert.deepEqual(netcoredbgCandidates("", { XDG_DATA_HOME: "/x" }), [path.join("/x", "kiln", "tools", "netcoredbg", "netcoredbg.exe")]);
    assert.throws(() => findNetcoredbg("/no/such/netcoredbg.exe", {}), (e: DebugRefused) => e.code === "tool_missing");
  });
  test("the bundled netcoredbg comes last, and an explicit path still never falls through to it", () => {
    const d = tmp();
    const bundled = path.join(d, "bundled.exe");
    fs.writeFileSync(bundled, "");
    const none = path.join(d, "no-data-dir");
    assert.equal(findNetcoredbg("", { KILN_DATA_DIR: none }, bundled), bundled);
    assert.throws(() => findNetcoredbg(path.join(d, "missing.exe"), { KILN_DATA_DIR: none }, bundled), (e: DebugRefused) => e.code === "tool_missing");
    assert.throws(() => findNetcoredbg("", { KILN_NETCOREDBG: path.join(d, "missing.exe"), KILN_DATA_DIR: none }, bundled), (e: DebugRefused) => e.code === "tool_missing");
    const installed = path.join(d, "data", "tools", "netcoredbg", "netcoredbg.exe");
    fs.mkdirSync(path.dirname(installed), { recursive: true });
    fs.writeFileSync(installed, "");
    assert.equal(findNetcoredbg("", { KILN_DATA_DIR: path.join(d, "data") }, bundled), installed);
  });
  test("the CLI checks the extension's own netcoredbg against the pin; an explicit choice only warns", () => {
    assert.deepEqual(netcoredbgEnv("/b/netcoredbg.exe", "", {}), { KILN_NETCOREDBG: "/b/netcoredbg.exe", KILN_NETCOREDBG_PINNED: "1" });
    assert.deepEqual(netcoredbgEnv("/s/netcoredbg.exe", "/s/netcoredbg.exe", {}), { KILN_NETCOREDBG: "/s/netcoredbg.exe" });
    assert.deepEqual(netcoredbgEnv("/e/netcoredbg.exe", "", { KILN_NETCOREDBG: "/e/netcoredbg.exe" }), { KILN_NETCOREDBG: "/e/netcoredbg.exe" });
  });
  test("a client is never carried by the built-in proxy (no pause limit, no identity re-check there)", () => {
    assert.equal(adapterKind("server", true), "cli");
    assert.equal(adapterKind("client", true), "cli");
    assert.equal(adapterKind("server", false), "proxy");
    assert.throws(() => adapterKind("client", false), (e: DebugRefused) => e.code === "tool_missing" && /--dap/.test(e.message));
  });
  test("reloads wait while a debugger is attached: this window's session, or a live kiln debug's session file", () => {
    const d = tmp();
    const file = path.join(d, "kiln", "state", "debug", "session.json");
    fs.mkdirSync(path.dirname(file), { recursive: true });
    const write = (state: string, proxyPid = process.pid, proxyStarted?: number) =>
      fs.writeFileSync(file, JSON.stringify({ format: 1, proxyPid, proxyStarted, state: { state, identity: { windowsPid: 32 } } }));
    assert.equal(reloadBlockedBy(d), undefined, "no file");
    write("paused", 4321, 1000);
    assert.deepEqual(attachedSession(d, () => true, () => 1001), { proxyPid: 4321, state: "paused" });
    assert.equal(attachedSession(d, () => false, () => 1001), undefined, "a proxy that is gone does not count");
    assert.equal(attachedSession(d, () => true, () => 5000), undefined, "another process under that PID does not count");
    write("detached", 4321, 1000);
    assert.equal(attachedSession(d, () => true, () => 1000), undefined, "an ended session does not count");
    write("running"); // this test process stands in for a live proxy
    assert.match(reloadBlockedBy(d)!, /kiln debug, pid \d+, running/);
    fs.rmSync(file);
    activeDebugTargets.add(path.resolve(d));
    try {
      assert.match(reloadBlockedBy(d)!, /this window/);
    } finally {
      activeDebugTargets.delete(path.resolve(d));
    }
    for (const c of ["reload m", "unload m", "reload-test m 20"]) assert.ok(unloadsModCode(c), c);
    for (const c of ["mods", "gc", "load folder", "test m", "reloadx"]) assert.ok(!unloadsModCode(c), c);
  });
  test("the fetch script's pin refuses any other file", () => {
    const d = tmp();
    const bogus = path.join(d, "netcoredbg-win64.zip");
    fs.writeFileSync(bogus, "not the release");
    const r = spawnSync(process.execPath, [path.join(EXT, "scripts", "fetch-netcoredbg.mjs"), "--check-zip", bogus], { encoding: "utf8" });
    assert.equal(r.status, 1);
    assert.match(r.stderr, /expected 3c410a45/);
  });
});

suite("dapProxy process", function () {
  this.timeout(30_000);
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  type LogLine = any;

  class Run {
    readonly log = path.join(tmp(), "dap.log");
    readonly msgs: DapMessage[] = [];
    readonly proc: ChildProcess;
    readonly exited: Promise<number | null>;
    private seq = 1;

    constructor(spec: Partial<ProxySpec> = {}, debuggerEnv: Record<string, string> = {}) {
      const full = { command: process.execPath, args: [MOCK], pid: 4242, ...spec, env: { KILN_MOCK_DAP_LOG: this.log, ...debuggerEnv, ...spec.env } };
      this.proc = spawn(process.execPath, [PROXY], { env: { ...process.env, ...debuggerEnv, KILN_DAP_SPEC: JSON.stringify(full), DISPLAY: ":99" }, stdio: ["pipe", "pipe", "pipe"] });
      const r = new DapReader((m) => this.msgs.push(m));
      this.proc.stdout!.on("data", (c: Buffer) => r.push(c));
      this.proc.stderr!.on("data", () => undefined);
      this.exited = new Promise((res) => this.proc.on("close", (code) => res(code)));
    }
    send(command: string, args?: object): number {
      const seq = this.seq++;
      this.proc.stdin!.write(encodeDap({ seq, type: "request", command, arguments: args }));
      return seq;
    }
    async until<T>(what: string, f: () => T | undefined | false, ms = 10_000): Promise<T> {
      const t0 = Date.now();
      for (;;) {
        const v = f();
        if (v) return v;
        if (Date.now() - t0 > ms) throw new Error(`timed out waiting for ${what}: ${JSON.stringify(this.msgs).slice(0, 600)}`);
        await sleep(20);
      }
    }
    req(command: string, args?: object): Promise<DapMessage> {
      const seq = this.send(command, args);
      return this.until(`${command} response`, () => this.msgs.find((m) => m.type === "response" && m.request_seq === seq));
    }
    lines(): LogLine[] {
      try {
        return fs.readFileSync(this.log, "utf8").split("\n").filter(Boolean).map((l) => JSON.parse(l));
      } catch {
        return [];
      }
    }
    async attached(): Promise<void> {
      await this.req("initialize", { adapterID: "kiln" });
      assert.equal((await this.req("attach", {})).success, true);
      await this.until("initialized", () => this.msgs.some((m) => m.event === "initialized"));
    }
    /** The outcome every path must have: a clean detach with terminateDebuggee:false, the debuggee alive. */
    assertDetached(): void {
      const l = this.lines();
      assert.deepEqual(l.filter((x) => x.kind === "debuggee-killed"), [], "the debuggee survives");
      const d = l.filter((x) => x.command === "disconnect");
      assert.equal(d.length, 1);
      assert.equal(d[0].arguments.terminateDebuggee, false);
      const kinds = l.map((x) => x.kind);
      assert.ok(kinds.indexOf("detached") >= 0 && kinds.indexOf("detached") < kinds.indexOf("exit"), `detached before exit: ${kinds.join(" ")}`);
      assert.equal(l.find((x) => x.kind === "exit").reason, "stdin closed", "ended by us closing its stdin, never by a signal");
    }
  }

  const runs: Run[] = [];
  const start = (spec?: Partial<ProxySpec>, env?: Record<string, string>) => {
    const r = new Run(spec, env);
    runs.push(r);
    return r;
  };
  teardown(async () => {
    for (const r of runs.splice(0)) {
      r.proc.stdin!.end();
      await r.exited;
    }
  });

  test("a whole session: capabilities patched, pid filled, Stop detaches, proxy exits 0", async () => {
    const r = start();
    const init = await r.req("initialize", {});
    assert.equal(init.body.supportsTerminateRequest, false);
    assert.equal(init.body.supportTerminateDebuggee, false);
    assert.equal(init.body.supportsConfigurationDoneRequest, true, "the rest passes through");
    await r.req("attach", { processId: 0 });
    assert.equal((await r.req("terminate", {})).success, false);
    assert.equal((await r.req("launch", {})).success, false);
    assert.equal((await r.req("attach", { processId: 7 })).success, false);
    assert.equal((await r.req("disconnect", { terminateDebuggee: true })).success, true);
    assert.equal(await r.exited, 0);
    r.assertDetached();
    const l = r.lines();
    assert.equal(l.filter((x) => x.command === "attach").length, 1, "the bad attach never reached it");
    assert.equal(l.find((x) => x.command === "attach").arguments.processId, 4242);
    const s = l.find((x) => x.kind === "start");
    assert.equal(s.pgidIsOwn, true);
    assert.equal(s.env.DISPLAY, null);
    assert.equal(s.env.ELECTRON_RUN_AS_NODE, null);
  });

  test("the client vanishes (stdin closes) while attached: detaches and waits for the answer", async () => {
    const r = start({}, { KILN_MOCK_DAP_SLOW_DISCONNECT_MS: "400" });
    await r.attached();
    r.proc.stdin!.end();
    assert.equal(await r.exited, 0);
    r.assertDetached();
  });

  for (const sig of ["SIGTERM", "SIGINT", "SIGHUP"] as const) {
    test(`${sig} to the proxy while attached: detaches, the debugger never sees the signal`, async () => {
      const r = start({}, { KILN_MOCK_DAP_SLOW_DISCONNECT_MS: "200" });
      await r.attached();
      r.proc.kill(sig);
      assert.equal(await r.exited, 0);
      r.assertDetached();
    });
  }

  test("stdin closes during a client disconnect: one disconnect, not two", async () => {
    const r = start({}, { KILN_MOCK_DAP_SLOW_DISCONNECT_MS: "300" });
    await r.attached();
    r.send("disconnect", {});
    await r.until("disconnect at the debugger", () => r.lines().some((x) => x.command === "disconnect"));
    r.proc.stdin!.end();
    assert.equal(await r.exited, 0);
    r.assertDetached();
  });

  test("not attached: the proxy goes without a disconnect", async () => {
    const r = start();
    await r.req("initialize", {});
    r.proc.stdin!.end();
    assert.equal(await r.exited, 0);
    assert.deepEqual(r.lines().filter((x) => x.command === "disconnect" || x.kind === "debuggee-killed"), []);
  });

  test("no answer to disconnect: it gives up after the timeout rather than hanging", async () => {
    const r = start({}, { KILN_MOCK_DAP_SLOW_DISCONNECT_MS: "60000", KILN_DAP_DISCONNECT_TIMEOUT_MS: "300" });
    await r.attached();
    r.proc.kill("SIGTERM");
    assert.equal(await Promise.race([r.exited, sleep(10_000).then(() => "hung")]), 0);
  });

  test("the debugger dies attached: the client is told and the session ends", async () => {
    const r = start({}, { KILN_MOCK_DAP_CRASH: "attach" });
    await r.req("initialize", {});
    r.send("attach", {});
    assert.equal(await r.exited, 0);
    assert.ok(r.msgs.some((m) => m.event === "terminated"));
    assert.ok(r.msgs.some((m) => m.event === "output" && /gone down/.test(m.body.output)), "the console says what it means for the server");
  });

  test("the debugger cannot start: every request gets the reason, until the client goes", async () => {
    const r = start({ command: "/no/such/wine", args: ["netcoredbg.exe"] });
    const init = await r.req("initialize", {});
    assert.equal(init.success, false);
    assert.match(init.message!, /\/no\/such\/wine/);
    const attach = await r.req("attach", {});
    assert.equal(attach.success, false);
    assert.equal((await r.req("disconnect", {})).success, true);
    r.proc.stdin!.end();
    assert.equal(await r.exited, 0);
  });
});
