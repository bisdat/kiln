import type { FoundReport, Profile } from "../api/commands";

export function sfSelfHostedText(file: string): string {
  return `ShroudForge's own loader is installed in the game folder (${file}), so ShroudForge runs its mods itself. Your profile's ShroudForge switches do not apply. Remove that file, or let Kiln manage ShroudForge, to have the profile decide.`;
}

export function sfSelfHostedFile(profile: Profile | null | undefined, found: FoundReport | null | undefined): string | null {
  if (!profile?.mods.shroudforge?.length) return null;
  return found?.loaders.find((l) => l.engine === "shroudforge")?.file ?? null;
}
