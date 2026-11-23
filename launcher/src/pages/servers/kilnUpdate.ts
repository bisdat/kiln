// The update rule of the Servers list: a server has a Kiln update when the Kiln in its folder is older than the newest
// runtime bundle the launcher has for servers (runtime_list). The installed version is the bundle's version
// (ServerStatus.installed, "0.2.0-dev.30.2"); "unknown" (Kiln put there by hand) and none (no Kiln: an install, not an
// update) never have one. A remote server has none either: server_deploy installs into a local folder only.
// Versions compare as semver with the pre-release's numeric parts as numbers (dev.30.2 < dev.30.4 < dev.30.10; a plain
// "dev" is older than "dev.30.2", as in semver), then by the build date older builds carried ("+20261104.0f1ffa6");
// runtime_list's order (newest first) breaks what is still a tie. shell/runtimeChange.ts' isOlder trusts the list
// first and drops the pre-release: an installed version whose bundle the launcher no longer keeps compares here too.
import type { RuntimeBundle } from "../../api/commands";

interface Parsed { main: number[]; pre: (string | number)[]; date: string }

function parse(v: string): Parsed | null {
  const [head, build = ""] = v.trim().split("+", 2);
  const dash = head.indexOf("-");
  const core = dash < 0 ? head : head.slice(0, dash);
  const pre = dash < 0 ? "" : head.slice(dash + 1);
  if (!/^\d+(\.\d+)*$/.test(core)) return null;
  return {
    main: core.split(".").map(Number),
    pre: pre ? pre.split(".").map((p) => (/^\d+$/.test(p) ? Number(p) : p)) : [],
    date: /^\d{8}/.exec(build)?.[0] ?? "",
  };
}

/** -1 when `a` is older than `b`, 1 when newer, 0 when the same (or the same as far as their words tell); null when
 *  either is not a version ("unknown"). */
export function compareKiln(a: string, b: string): -1 | 0 | 1 | null {
  const x = parse(a), y = parse(b);
  if (!x || !y) return null;
  for (let i = 0; i < Math.max(x.main.length, y.main.length); i++) {
    const p = x.main[i] ?? 0, q = y.main[i] ?? 0;
    if (p !== q) return p < q ? -1 : 1;
  }
  // a release is newer than any of its pre-releases
  if (!x.pre.length !== !y.pre.length) return x.pre.length ? -1 : 1;
  for (let i = 0; i < Math.max(x.pre.length, y.pre.length); i++) {
    const p = x.pre[i], q = y.pre[i];
    if (p === undefined) return -1;
    if (q === undefined) return 1;
    if (p === q) continue;
    if (typeof p === "number" && typeof q === "number") return p < q ? -1 : 1;
    if (typeof p === "number") return -1;
    if (typeof q === "number") return 1;
    return p < q ? -1 : 1;
  }
  if (x.date && y.date && x.date !== y.date) return x.date < y.date ? -1 : 1;
  return 0;
}

/** The newest bundle with the server side (ties: runtime_list's order, newest first). */
export function newestServerBundle(bundles: RuntimeBundle[] | undefined): RuntimeBundle | null {
  let best: RuntimeBundle | null = null;
  for (const b of bundles ?? []) {
    if (!b.sides.includes("server")) continue;
    if (!best || compareKiln(b.version, best.version) === 1) best = b;
  }
  return best;
}

/** The version a server's Kiln can be updated to, or null: none installed, "unknown", remote, or already the newest. */
export function kilnUpdateFor(installed: string | null | undefined, bundles: RuntimeBundle[] | undefined, remote = false): string | null {
  if (remote || !installed || installed === "unknown") return null;
  const newest = newestServerBundle(bundles);
  if (!newest || newest.version === installed) return null;
  const c = compareKiln(installed, newest.version);
  if (c === -1) return newest.version;
  if (c === 0) {
    const list = bundles ?? [];
    const i = list.findIndex((b) => b.version === installed), j = list.indexOf(newest);
    return i >= 0 && j >= 0 && i > j ? newest.version : null;
  }
  return null;
}

/** A version as the list shows it: without the build metadata ("0.2.0-dev+20261104.0f1ffa6" reads "0.2.0-dev"). */
export function shortKiln(v: string | null | undefined): string | null {
  if (!v) return null;
  return v.split("+")[0];
}
