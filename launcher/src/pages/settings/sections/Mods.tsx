// Settings > Mods & profiles: the profiles (and Crucible's packs), how new mods behave, the library and its data folder.
import { useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import { KEEP_MAX, type LauncherSettings } from "../../../api/commands";
import { useSettings } from "../../../api/hooks";
import { LibraryCard } from "../../../components/LibrarySettings";
import { ProfilesCard } from "../../../components/ProfilesSettings";
import { CruciblePacksCard } from "../../../crucible/Profiles";
import { useCrucibleOn } from "../../../crucible/state";
import { saveSettings } from "../../../deck/mode";
import { NumberInput, Toggle, setTypeToConfirm, typeToConfirm } from "../../../ui";
import { CardGroup, Group, SettingRow } from "../parts";

export function ModsSection() {
  const crucible = useCrucibleOn();
  return (
    <>
      <CardGroup id="profiles"><ProfilesCard /></CardGroup>
      {crucible && <CardGroup id="crucible"><CruciblePacksCard /></CardGroup>}
      <AddingGroup />
      <CardGroup id="library" items={["library", "keepVersions"]}>
        <LibraryCard><KeepVersionsRow /></LibraryCard>
      </CardGroup>
    </>
  );
}

/** Saves a change of the launcher's settings (deck/mode.tsx saveSettings: in press order). */
export function useSetSettings() {
  const qc = useQueryClient();
  return (patch: Partial<LauncherSettings>) => void saveSettings(qc, () => patch);
}

/** Switch new mods on, folder notes, the daily mod update check, typing a mod's name to remove it. */
function AddingGroup() {
  const s = useSettings();
  const set = useSetSettings();
  const v = s.data;
  return (
    <Group id="adding" items={["addToActive", "folderNotes", "dailyUpdates", "typeToConfirm"]}>
      <SettingRow id="addToActive" htmlFor="st-add-active"
        control={<Toggle id="st-add-active" checked={v?.addToActiveProfile !== false} disabled={!v} onChange={(on) => set({ addToActiveProfile: on })} />} />
      <SettingRow id="folderNotes" htmlFor="st-folder-notes"
        control={<Toggle id="st-folder-notes" checked={v?.modFolderNotes !== false} disabled={!v} onChange={(on) => set({ modFolderNotes: on })} />} />
      <SettingRow id="dailyUpdates" htmlFor="st-daily-updates"
        control={<Toggle id="st-daily-updates" checked={v?.modUpdatesDaily === true} disabled={!v} onChange={(on) => set({ modUpdatesDaily: on })} />} />
      <TypeToConfirmRow />
    </Group>
  );
}

/** "Type a mod's name to remove it" (off unless the player turns it on): off = a mod's Remove has a plain button. The
 * dialogs that uninstall Kiln, delete a profile or a server, or restore a backup always ask for the word. */
function TypeToConfirmRow() {
  const [on, setOn] = useState(typeToConfirm());
  return (
    <SettingRow id="typeToConfirm" htmlFor="st-type-confirm"
      control={<Toggle id="st-type-confirm" checked={on} onChange={(v) => { setTypeToConfirm(v); setOn(v); }} />} />
  );
}

/** Per mod, at an update: its earlier version and the settings it had, for Roll back in its menu. */
function KeepVersionsRow() {
  const s = useSettings();
  const set = useSetSettings();
  if (!s.data) return null;
  return (
    <SettingRow id="keepVersions" htmlFor="st-keep-versions"
      control={<NumberInput id="st-keep-versions" integer min={0} max={KEEP_MAX} value={s.data.keepVersions ?? 1} onCommit={(n) => set({ keepVersions: n })} style={{ width: 96 }} />} />
  );
}
