// SPDX-License-Identifier: MIT
// What the debugger attaches to comes from one place: `kiln debug --json` (kiln_manage::debug, the shared resolver).
// It proves the target by its Windows identity (PID, creation time, image digest, session nonce) and refuses with a
// stable code; nothing here lists processes, runs tasklist or guesses a PID. This file turns a debug configuration
// into the CLI's arguments, reads its answer, and describes the session states the CLI sends while attached.
import * as fs from "node:fs";
import * as path from "node:path";
import { ProxySpec } from "./dap";
import { kilnDataDir } from "./dataDir";
import { run } from "./proc";
import { expandHome } from "./servers";

/** The CLI's stable refusal codes (kiln_manage::debug::DebugCode, the CLI's own), and the extension's usage error. */
export type DebugCode =
  | "refused"
  | "not_running"
  | "identity_changed"
  | "runtime_not_ready"
  | "tool_missing"
  | "privilege_mismatch"
  | "ambiguous_runner"
  | "unsupported_identity"
  | "pause_consent_required"
  | "not_found"
  | "unsupported"
  | "timeout"
  | "usage";

const CODES = new Set<string>([
  "refused", "not_running", "identity_changed", "runtime_not_ready", "tool_missing", "privilege_mismatch", "ambiguous_runner",
  "unsupported_identity", "pause_consent_required", "not_found", "unsupported", "timeout", "usage",
]);

export class DebugRefused extends Error {
  constructor(
    readonly code: DebugCode,
    message: string,
    readonly reason?: string,
  ) {
    super(message);
  }
}

/** The fields of a `kiln` debug configuration (package.json `debuggers`). */
export interface KilnDebugConfig {
  server?: string;
  client?: string;
  allowClientPause?: boolean;
  maxPauseMs?: number;
  sourceFileMap?: Record<string, string>;
  justMyCode?: boolean;
}

export type Selection = { kind: "server"; server?: string } | { kind: "client"; client: string };

/**
 * Exactly one of `server` / `client`; a client only with `allowClientPause: true` (every attach can stop the game);
 * `maxPauseMs` 100 to 60000. Checked before anything runs: a refusal here starts no process.
 */
export function selectTarget(c: KilnDebugConfig): Selection {
  const has = (v: unknown) => typeof v === "string" && v.trim() !== "";
  if (has(c.server) && has(c.client)) throw new DebugRefused("usage", "a Kiln debug configuration names one target: `server` or `client`, not both.");
  if (c.maxPauseMs !== undefined && (!Number.isInteger(c.maxPauseMs) || c.maxPauseMs < 100 || c.maxPauseMs > 60_000)) {
    throw new DebugRefused("usage", `maxPauseMs is a whole number from 100 to 60000 (got ${c.maxPauseMs}).`);
  }
  if (has(c.client)) {
    if (c.allowClientPause !== true) {
      throw new DebugRefused(
        "pause_consent_required",
        "a debugger stops the game at every breakpoint, and a stopped game can freeze or lose its connection. Set \"allowClientPause\": true in this launch configuration to attach to a game client, or watch its log with `kiln debug --client <install> --observe`.",
      );
    }
    return { kind: "client", client: c.client!.trim() };
  }
  return { kind: "server", server: has(c.server) ? c.server!.trim() : undefined };
}

/** `kiln debug` arguments for a resolved selection (`server`: the server's folder; `client`: an install id or folder). */
export function debugArgs(sel: { kind: "server"; dir: string } | { kind: "client"; client: string }, c: KilnDebugConfig): string[] {
  const a = sel.kind === "server" ? ["debug", "--server", sel.dir] : ["debug", "--client", sel.client, "--allow-client-pause"];
  if (c.maxPauseMs !== undefined) a.push("--max-pause-ms", String(c.maxPauseMs));
  return a;
}

/** What `kiln debug --json` resolved (the fields the extension uses; DebugTarget is kiln_manage::debug's). */
export interface CliResolution {
  kind: "server" | "client";
  install: string;
  pid: number;
  hostPid?: number | null;
  wine?: string | null;
  prefix?: string | null;
  netcoredbg: string;
  env: Record<string, string>;
  command: string[];
  maxPauseMs?: number | null;
  target: {
    identity: { windowsPid: number; creationTime100ns: string; imageSha256: string; sessionNonce: string; installId: string };
    sessionMode: string;
    managedReady: boolean;
    runtimeVersion: string;
  };
}

/** The CLI's answer: a resolution, or a DebugRefused with its stable code and reason. */
export function parseResolution(stdout: string): CliResolution {
  let v: any;
  try {
    v = JSON.parse(stdout);
  } catch {
    throw new DebugRefused("tool_missing", "`kiln debug --json` did not answer with JSON: is the kiln CLI up to date (kiln.cli.path)?");
  }
  if (v?.ok === false) {
    const code = typeof v.error?.code === "string" && CODES.has(v.error.code) ? (v.error.code as DebugCode) : "refused";
    throw new DebugRefused(code, v.error?.message ?? "kiln debug refused the target.", v.error?.detail?.reason);
  }
  if (v?.ok !== true || !v.target?.identity || typeof v.pid !== "number" || !Array.isArray(v.command)) {
    throw new DebugRefused("tool_missing", "this kiln CLI does not prove debug targets yet (no identity in `kiln debug --json`); update Kiln's SDK.");
  }
  return v as CliResolution;
}

/** Runs `kiln debug … --json` and reads its answer. */
export async function resolveWithCli(cli: string, args: string[], env: Record<string, string>, timeoutMs = 60_000): Promise<CliResolution> {
  const r = await run(cli, [...args, "--json"], { timeoutMs, env: { ...process.env, ...env } });
  if (r.error && !r.stdout.trim()) throw new DebugRefused("tool_missing", `cannot run ${cli}: ${r.error.message}`);
  return parseResolution(r.stdout);
}

/** One line for the output channel: what was proven. */
export function describeResolution(r: CliResolution): string {
  const id = r.target.identity;
  return `${r.kind} ${r.install}: Windows PID ${id.windowsPid} (created ${id.creationTime100ns}), session ${id.sessionNonce.slice(0, 8)}…, mode ${r.target.sessionMode}, .NET ${r.target.runtimeVersion || "?"}${r.hostPid ? `, host pid ${r.hostPid} (display only)` : ""}`;
}

/**
 * The built-in proxy as a transport for a target the CLI resolved: the CLI's own command line, environment and PID
 * (it never chooses a target of its own).
 */
export function proxySpecFromCli(r: CliResolution): ProxySpec {
  const [command, ...args] = r.command;
  return { command, args, env: { ...r.env }, cwd: path.dirname(r.netcoredbg), pid: r.pid };
}

/** A `kiln/debugState` event (kiln_manage::debug::DebugSessionState) as a line, and whether to show it to the user. */
export function describeDebugState(b: any): { text: string; notify: "none" | "info" | "warn" } {
  const state = String(b?.state ?? "?");
  const reason = typeof b?.reason === "string" ? b.reason : undefined;
  const mode = String(b?.sessionMode ?? "unknown");
  const pid = b?.identity?.windowsPid;
  const text = `debug: ${state}${reason ? ` (${reason})` : ""}, PID ${pid ?? "?"}, mode ${mode}${b?.maxPauseMs ? `, pauses end after ${b.maxPauseMs} ms` : ""}`;
  if (reason === "timeout_continue") return { text, notify: "warn" };
  if (state === "ended") return { text, notify: "warn" };
  if (reason === "session_mode_changed") return { text, notify: "info" };
  return { text, notify: "none" };
}

/** The extension's message for a refusal: the CLI's sentence, and what to do for the codes that have a fix. */
export function refusalMessage(e: DebugRefused): string {
  switch (e.code) {
    case "pause_consent_required":
      return e.message;
    case "refused":
      return e.reason === "developer_debugging_off"
        ? `${e.message} (the launcher's Settings: Developer debugging)`
        : e.message;
    case "tool_missing":
      return e.message;
    default:
      return `${e.message} [${e.code}${e.reason ? `: ${e.reason}` : ""}]`;
  }
}

/**
 * kiln.debug.netcoredbg, then $KILN_NETCOREDBG (an explicit path never falls through), then
 * <Kiln data dir>/tools/netcoredbg/netcoredbg.exe as `kiln debug` looks it up, then the copy bundled with the extension.
 */
export function netcoredbgCandidates(setting: string | undefined, env: NodeJS.ProcessEnv = process.env, bundled?: string): string[] {
  const explicit = setting || env.KILN_NETCOREDBG;
  if (explicit) return [expandHome(explicit)];
  const data = kilnDataDir(env);  // a data folder moved in the launcher is followed through its pointer
  return [path.join(data, "tools", "netcoredbg", "netcoredbg.exe"), ...(bundled ? [bundled] : [])];
}

export function findNetcoredbg(setting: string | undefined, env: NodeJS.ProcessEnv = process.env, bundled?: string): string {
  const c = netcoredbgCandidates(setting, env, bundled);
  const found = c.find((p) => fs.existsSync(p));
  if (!found) {
    throw new DebugRefused("tool_missing", `no netcoredbg at ${c.join(" or ")}. The .vsix bundles it; from source, run npm run fetch:netcoredbg in tools/vscode, or set kiln.debug.netcoredbg.`);
  }
  return found;
}

/**
 * The environment for `kiln debug` with the netcoredbg the extension found: KILN_NETCOREDBG always; KILN_NETCOREDBG_PINNED=1
 * when it is the extension's own bundled copy or the Kiln data folder's (the CLI then refuses one that is not the pinned
 * release), never for the user's explicit setting or $KILN_NETCOREDBG (a mismatch there only warns).
 */
export function netcoredbgEnv(found: string, setting: string | undefined, env: NodeJS.ProcessEnv = process.env): Record<string, string> {
  const explicit = !!(setting || env.KILN_NETCOREDBG);
  return explicit ? { KILN_NETCOREDBG: found } : { KILN_NETCOREDBG: found, KILN_NETCOREDBG_PINNED: "1" };
}

/**
 * Which adapter carries a session: `kiln debug … --dap` when the CLI has it; else the built-in proxy, for servers only.
 * A client needs the CLI's adapter (its pause limit, the identity check right before the attach, the session file and
 * the session states): the built-in proxy has none of them, so a client without it is refused.
 */
export function adapterKind(kind: "server" | "client", cliHasDap: boolean): "cli" | "proxy" {
  if (cliHasDap) return "cli";
  if (kind === "client") {
    throw new DebugRefused("tool_missing", "debugging a game client needs `kiln debug --dap` (its pause limit and identity checks), and this kiln CLI has no --dap: update Kiln's SDK (kiln.cli.path).");
  }
  return "proxy";
}

/** Server folders with a debug session of this window (the built-in proxy writes no session file). */
export const activeDebugTargets = new Set<string>();

const ATTACHED = new Set(["attaching", "running", "paused", "detaching"]);

/** Seconds since 1970 that /proc says a process started at (Linux), or undefined. */
function linuxStartSeconds(pid: number): number | undefined {
  try {
    const stat = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
    const ticks = Number(stat.slice(stat.lastIndexOf(")") + 2).split(" ")[19]);
    const btime = Number(/^btime (\d+)$/m.exec(fs.readFileSync("/proc/stat", "utf8"))?.[1]);
    return Number.isFinite(ticks) && Number.isFinite(btime) ? Math.floor(btime + ticks / 100) : undefined;
  } catch {
    return undefined;
  }
}

/**
 * The attached session `kiln debug --dap` recorded in `<install>/kiln/state/debug/session.json`, when its proxy still
 * runs (the same PID, and on Linux the same start, as kiln_manage::debug::attached_session checks); undefined otherwise.
 */
export function attachedSession(installDir: string, alive: (pid: number) => boolean = pidAlive, started: (pid: number) => number | undefined = linuxStartSeconds): { proxyPid: number; state: string } | undefined {
  let f: any;
  try {
    f = JSON.parse(fs.readFileSync(path.join(installDir, "kiln", "state", "debug", "session.json"), "utf8"));
  } catch {
    return undefined;
  }
  const state = String(f?.state?.state ?? "");
  const pid = Number(f?.proxyPid);
  if (!ATTACHED.has(state) || !Number.isInteger(pid) || pid <= 0 || !alive(pid)) return undefined;
  const now = started(pid);
  if (now !== undefined && typeof f.proxyStarted === "number" && Math.abs(now - f.proxyStarted) > 2) return undefined;
  return { proxyPid: pid, state };
}

function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return (e as NodeJS.ErrnoException).code === "EPERM";
  }
}

/** Control commands that unload mod code, refused while a debugger is attached (the host refuses them too). */
export function unloadsModCode(command: string): boolean {
  return /^(reload|unload|reload-test)(\s|$)/.test(command.trim());
}

/** Why a reload (or unload) of mods on this server must wait, or undefined: a debug session of this window, or one
 *  `kiln debug` recorded for that server. */
export function reloadBlockedBy(serverDir: string): string | undefined {
  if (activeDebugTargets.has(path.resolve(serverDir))) return "a debugger of this window is attached to this server: stop the debug session first";
  const s = attachedSession(serverDir);
  return s ? `a debugger is attached to this server (kiln debug, pid ${s.proxyPid}, ${s.state}): disconnect it first` : undefined;
}
