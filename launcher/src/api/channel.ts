// The launcher's update channels as players see them (selfupdate/feed.rs, Channel). The stored values stay "stable",
// "dev" and "beta" (settings.json `updateChannel`; a 0.2.0-dev.28 launcher reads "stable" and "dev" as before and
// follows its own channel for "beta"). Every public release so far is a dev build (`X.Y.Z-dev.N`), so the dev line is
// the release players have: it is called "Release" until a stable release exists (STABLE_RELEASED). "Stable" is offered
// only where it is already chosen (it has no builds yet).
import type { UpdateChannel } from "./commands";

/** Is there a stable release (an `X.Y.Z` without `-dev.N`)? Flip with the first one: then "stable" is the Release and
 *  the dev line becomes "Dev builds". */
export const STABLE_RELEASED = false;

/** The channel's name in the UI. */
export function channelName(ch: UpdateChannel): string {
  switch (ch) {
    case "beta": return "Beta (test builds)";
    case "stable": return STABLE_RELEASED ? "Release" : "Stable (none yet)";
    default: return STABLE_RELEASED ? "Dev builds" : "Release";
  }
}

/** The short word for marks and rows: "Release", "Beta". */
export function channelWord(ch: UpdateChannel): string {
  return ch === "beta" ? "Beta" : ch === "stable" ? (STABLE_RELEASED ? "Release" : "Stable") : STABLE_RELEASED ? "Dev" : "Release";
}

/** The release line a tester goes back to: the stable channel once there is a stable release, else dev. */
export const RELEASE_CHANNEL: UpdateChannel = STABLE_RELEASED ? "stable" : "dev";

/** The choices of Settings > Updates > Channel, the release line first. `current`: the channel in use (a "stable" chosen
 *  before there is a stable release stays listed, so the select shows it). */
export function channelOptions(current: UpdateChannel): { value: UpdateChannel; label: string }[] {
  const order: UpdateChannel[] = STABLE_RELEASED ? ["stable", "dev", "beta"] : ["dev", "beta", ...(current === "stable" ? ["stable" as const] : [])];
  return order.map((value) => ({ value, label: channelName(value) }));
}

/** A build id as the updater writes it (selfupdate/feed.rs, Build): `0.2.0-dev.29+beta.3` is test build 3 of
 *  0.2.0-dev.29, the beta number as build metadata; any other build metadata is not a beta. */
export function parseBuild(id: string): { version: string; beta: number | null } {
  const m = /^([^+]+)\+beta\.([1-9][0-9]{0,8})$/.exec(id.trim());
  return m ? { version: m[1], beta: Number(m[2]) } : { version: id.trim(), beta: null };
}

/** A build for people: "0.2.0-dev.29 beta 3", "0.2.0-dev.28". */
export function buildLabel(id: string, beta: number | null = null): string {
  const b = parseBuild(id);
  const n = b.beta ?? beta;
  return n ? `${b.version} beta ${n}` : b.version;
}

/** What choosing `target` means on this build: "to-beta" (a release build installs the newest test build now), "back"
 *  (a test build installs the newest release now, also when it is older), or null (only the setting changes: the next
 *  update follows it). */
export type SwitchKind = "to-beta" | "back";
export function switchKind(s: { buildChannel?: UpdateChannel | null }, target: UpdateChannel): SwitchKind | null {
  const build = s.buildChannel ?? "dev";
  if (target === "beta" && build !== "beta") return "to-beta";
  if (target !== "beta" && build === "beta") return "back";
  return null;
}

/** Said before every switch: worlds and multiplayer. */
export const WORLD_WARNING = "A world you play with a test build may not load the same way on the release, and servers and players must run the same build to play together.";

/** The switch dialog's words (desktop and Deck). */
export function switchWords(kind: SwitchKind): { title: string; say: string[]; confirm: string; task: string } {
  return kind === "to-beta"
    ? {
      title: "Switch to the beta?",
      say: [
        "Test builds of the next release, for testing with the Kiln Discord. The launcher installs the newest one and restarts; Kiln in your game follows at the next Play.",
        "You can switch back to the release here at any time.",
        WORLD_WARNING,
      ],
      confirm: "Switch now",
      task: "Switch to the beta",
    }
    : {
      title: "Switch back to the release?",
      say: [
        "The launcher installs the newest release and restarts, also when it is older than this test build. Kiln in your game goes back with it at the next Play.",
        WORLD_WARNING,
      ],
      confirm: "Switch back",
      task: "Switch back to the release",
    };
}

/** The restore choice of a switch back (a "before beta" copy exists). */
export function restoreWords(taken: string): { label: string; hint: string } {
  const d = new Date(taken);
  const when = Number.isNaN(d.getTime()) ? taken : d.toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" });
  return {
    label: "Put back my profiles and settings from before the beta",
    hint: `Saved ${when}, before the first start of a test build. Mods you added in the beta stay in the Library.`,
  };
}

/** The Play page's notice when the game's Kiln and the launcher's channel differ, or null.
 *  `switchBack`: the game's Kiln came with a test build and this launcher's own is older (InstallState.switchBack).
 *  `betaLauncher`: this launcher is a test build and has a newer Kiln than the game's (upgradeAvailable). */
export function channelNotice(rt: { installed: boolean; upgradeAvailable: string | null; switchBack?: { from: string; to: string } | null } | null | undefined,
                              betaLauncher: boolean, label: (v: string) => string = (v) => v): string | null {
  if (!rt?.installed) return null;
  if (rt.switchBack) return `This game has Kiln ${label(rt.switchBack.from)} from a test build; Play puts this launcher's Kiln ${label(rt.switchBack.to)} in first.`;
  if (betaLauncher && rt.upgradeAvailable) return `This launcher is a test build; this game still has the release's Kiln. Install Kiln ${label(rt.upgradeAvailable)} on Runtime to test it.`;
  return null;
}
