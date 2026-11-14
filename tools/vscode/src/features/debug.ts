// SPDX-License-Identifier: MIT
// Debugging mods: the "kiln" attach configuration, for a test server (`server`) or a game client (`client`, only with
// `allowClientPause: true`). The target always comes from the shared resolver, `kiln debug … --json`, which proves it by
// its Windows identity and refuses with a stable code; the adapter is `kiln debug … --dap` (consent, the whole identity
// again before attaching, the pause watchdog, session states as `kiln/debugState` events). The built-in proxy
// (dist/dapProxy.js) is only a transport for a target the CLI resolved, for a CLI without `--dap`; it never picks one.
import * as path from "node:path";
import * as vscode from "vscode";
import { sendControl } from "../core/control";
import { staleContextsWarning } from "../core/dap";
import {
  CliResolution, DebugRefused, debugArgs, describeDebugState, describeResolution, findNetcoredbg, KilnDebugConfig, netcoredbgCandidates,
  proxySpecFromCli, refusalMessage, resolveWithCli, selectTarget,
} from "../core/debugTarget";
import { discoverLabs, expandHome, ServerDef } from "../core/servers";
import { Ctx } from "../ctx";
import { Servers } from "./servers";

const DOC = ["docs", "kiln", "modding", "debugging.md"];
/** The key a session is prepared and tracked under (`server:<dir>` or `client:<install>`). */
const KEY = "kilnTarget";

interface Prepared {
  descriptor: vscode.DebugAdapterDescriptor;
}

export class DebugFeature implements vscode.DebugConfigurationProvider, vscode.DebugAdapterDescriptorFactory, vscode.Disposable {
  private prepared = new Map<string, Prepared>();
  private active = new Map<string, vscode.DebugSession>();
  private disposables: vscode.Disposable[] = [];

  constructor(
    private readonly ctx: Ctx,
    private readonly servers: Servers,
  ) {
    this.disposables.push(
      vscode.debug.registerDebugConfigurationProvider("kiln", this),
      vscode.debug.registerDebugAdapterDescriptorFactory("kiln", this),
      vscode.debug.onDidStartDebugSession((s) => {
        if (s.type === "kiln" && s.configuration[KEY]) this.active.set(s.configuration[KEY], s);
      }),
      vscode.debug.onDidTerminateDebugSession((s) => {
        if (s.type === "kiln" && this.active.get(s.configuration[KEY]) === s) this.active.delete(s.configuration[KEY]);
      }),
      vscode.debug.onDidReceiveDebugSessionCustomEvent((e) => {
        if (e.session.type !== "kiln" || e.event !== "kiln/debugState") return;
        const d = describeDebugState(e.body);
        this.ctx.out.info(d.text);
        if (d.notify === "warn") void vscode.window.showWarningMessage(`Kiln ${d.text}`);
        else if (d.notify === "info") void vscode.window.setStatusBarMessage(`Kiln ${d.text}`, 5000);
      }),
      vscode.commands.registerCommand("kiln.server.debug", (s?: ServerDef) => this.attach(s)),
    );
  }

  dispose(): void {
    for (const d of this.disposables) d.dispose();
  }

  provideDebugConfigurations(): vscode.DebugConfiguration[] {
    return [{ type: "kiln", request: "attach", name: "Kiln: attach to server" }];
  }

  /** Fills the defaults (F5 without a launch.json comes here with an empty configuration). */
  resolveDebugConfiguration(_folder: vscode.WorkspaceFolder | undefined, config: vscode.DebugConfiguration): vscode.DebugConfiguration {
    return { ...config, type: config.type || "kiln", request: config.request || "attach", name: config.name || "Kiln: attach to server" };
  }

  /** The checks, on the final configuration. Returning undefined aborts the session: nothing has started. */
  async resolveDebugConfigurationWithSubstitutedVariables(_folder: vscode.WorkspaceFolder | undefined, config: vscode.DebugConfiguration): Promise<vscode.DebugConfiguration | undefined> {
    try {
      const c = config as KilnDebugConfig;
      const sel = selectTarget(c); // consent and the one-target rule: before any process runs
      let key: string;
      let args: string[];
      let def: ServerDef | undefined;
      if (sel.kind === "client") {
        key = `client:${sel.client}`;
        args = debugArgs({ kind: "client", client: sel.client }, c);
      } else {
        def = await this.server(sel.server);
        if (!def) return undefined;
        key = `server:${def.dir}`;
        args = debugArgs({ kind: "server", dir: def.dir }, c);
      }
      if (this.active.has(key)) throw new DebugRefused("refused", `a debugger is already attached to ${def?.name ?? (c.client ?? "")}.`);
      this.prepared.set(key, { descriptor: await this.descriptor(args, def) });
      return { ...config, ...(def ? { server: def.dir } : {}), [KEY]: key };
    } catch (e) {
      if (!(e instanceof DebugRefused)) throw e;
      this.ctx.out.warn(`debug: ${e.code}${e.reason ? ` (${e.reason})` : ""}: ${e.message}`);
      void this.explain(e);
      return undefined;
    }
  }

  async createDebugAdapterDescriptor(session: vscode.DebugSession): Promise<vscode.DebugAdapterDescriptor | undefined> {
    const key: string = session.configuration[KEY];
    const p = this.prepared.get(key);
    this.prepared.delete(key);
    if (!p) throw new Error(`Kiln: session for an unprepared target (${key})`);
    return p.descriptor;
  }

  private bundledNetcoredbg(): string {
    return this.ctx.asset("dist", "netcoredbg", "netcoredbg.exe");
  }

  /** Test Servers view: attach to this server (or the selected one). */
  private attach(s?: ServerDef): Thenable<boolean> {
    const d = s ?? this.servers.selected();
    return vscode.debug.startDebugging(vscode.workspace.workspaceFolders?.[0], {
      type: "kiln",
      request: "attach",
      name: d ? `Kiln: attach to ${d.name}` : "Kiln: attach to server",
      ...(d ? { server: d.dir } : {}),
    });
  }

  /** `server` from launch.json: a name, a folder or a lab name; empty: the selected server. */
  private async server(v: unknown): Promise<ServerDef | undefined> {
    if (typeof v !== "string" || !v) return this.servers.require("attach the debugger to");
    const all = this.servers.list();
    const dir = path.resolve(expandHome(v));
    const def =
      all.find((s) => s.name === v) ??
      all.find((s) => path.resolve(s.dir) === dir) ??
      discoverLabs().find((s) => s.name === v || path.resolve(s.dir) === dir);
    if (def) return def;
    // any other folder: `kiln debug` decides whether it knows that server (a lab or a server instance) or refuses
    return { name: path.basename(dir), dir, kind: "folder", source: "settings" };
  }

  /**
   * The target from `kiln debug … --json` (refusals arrive here, before any adapter starts, with the CLI's code), then
   * the adapter: `kiln debug … --dap`, or for a CLI without `--dap` the built-in proxy around the CLI's own command.
   */
  private async descriptor(args: string[], def: ServerDef | undefined): Promise<vscode.DebugAdapterDescriptor> {
    if (!(await this.ctx.hasCli())) {
      throw new DebugRefused("tool_missing", `debugging needs the kiln CLI (kiln.cli.path is ${this.ctx.cliPath()}): it proves which process the debugger attaches to.`);
    }
    const dbg = findNetcoredbg(this.ctx.setting("debug.netcoredbg", ""), process.env, this.bundledNetcoredbg());
    const env = { KILN_NETCOREDBG: dbg }; // the CLI alone has no bundled copy to fall back on
    const cli = this.ctx.cliPath();
    const r: CliResolution = await resolveWithCli(cli, args, env);
    this.ctx.out.info(`debug: ${describeResolution(r)}`);
    const help = await this.ctx.exec(cli, ["debug", "--help"], { timeoutMs: 5000 });
    if (!help.error && help.code === 0 && help.stdout.includes("--dap")) {
      this.ctx.out.info(`debug: ${cli} ${[...args, "--dap"].join(" ")}`);
      return new vscode.DebugAdapterExecutable(cli, [...args, "--dap"], { env });
    }
    if (def) await this.collect(def);
    // "node": VS Code forks its own Node for this, no node on PATH needed
    return new vscode.DebugAdapterExecutable("node", [this.ctx.asset("dist", "dapProxy.js")], {
      env: { KILN_DAP_SPEC: JSON.stringify(proxySpecFromCli(r)) },
    });
  }

  /**
   * A reload leaves the old mod's module loaded until the next collection, and netcoredbg can bind a breakpoint to that
   * dead copy (1 of 6 attaches stopped without this, 6 of 6 with it). `kiln debug --dap` does the same itself.
   */
  private async collect(def: ServerDef): Promise<void> {
    try {
      const r = await sendControl(def.dir, "gc", { timeoutMs: 30_000 });
      const warning = staleContextsWarning(r.message);
      if (warning) void vscode.window.showWarningMessage(`Kiln: ${warning}`);
    } catch (e) {
      this.ctx.out.warn(`debug: no collection before attaching (${(e as Error).message}); after a reload, breakpoints may not stop`);
    }
  }

  private async explain(e: DebugRefused): Promise<void> {
    const act = await vscode.window.showErrorMessage(`Kiln: ${refusalMessage(e)}`, ...(e.code === "tool_missing" || e.code === "pause_consent_required" ? ["Open docs"] : []));
    const root = this.ctx.kilnRoot();
    if (act && root) void vscode.window.showTextDocument(vscode.Uri.file(path.join(root, ...DOC)));
    else if (act) this.ctx.out.info(`netcoredbg goes to: ${netcoredbgCandidates(this.ctx.setting("debug.netcoredbg", ""), process.env, this.bundledNetcoredbg()).join(", ")}`);
  }
}
