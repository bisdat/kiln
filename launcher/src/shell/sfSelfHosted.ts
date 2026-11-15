import type { FoundReport, Profile } from "../api/commands";

export function sfSelfHostedText(file: string): string {
  return `ShroudForge's own loader is installed (${file}). If it loads, Kiln's adapter steps aside and ShroudForge reads its own mods folder. Profile switches still update copies Kiln manages. Folders you installed yourself keep their own switches. Remove that loader or let Kiln manage ShroudForge to use Kiln's adapter.`;
}

export function sfSelfHostedFile(profile: Profile | null | undefined, found: FoundReport | null | undefined): string | null {
  if (!profile?.mods.shroudforge?.length) return null;
  return found?.loaders.find((l) => l.engine === "shroudforge")?.file ?? null;
}
