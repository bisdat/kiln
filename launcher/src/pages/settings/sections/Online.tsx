// Settings > Updates & online: launcher updates and the channel (or, in the Nexus build, where new versions are), the
// Nexus Mods account, the Community page (Discord), and what Kiln sends (Crucible's install counts).
import { useMutation, useQueryClient } from "@tanstack/react-query";
import * as api from "../../../api/commands";
import type { LauncherSettings, UpdateChannel } from "../../../api/commands";
import { RELEASE_CHANNEL, channelOptions, switchKind } from "../../../api/channel";
import { useDistribution, useNexusEnabled, useSettings } from "../../../api/hooks";
import { useUpdate, useUpdateStore } from "../../../api/update";
import { setCommunity, useCommunitySettings } from "../../../community/state";
import { NexusCard } from "../../../components/NexusSettings";
import { NexusTestBuildsLink, NexusVersionsLink, updateWords, useCheckNow } from "../../../components/UpdateSettings";
import { useCrucibleOn } from "../../../crucible/state";
import { saveSettings } from "../../../deck/mode";
import { rich, useT } from "../../../i18n";
import { useChannelSwitch } from "../../../shell/ChannelSwitch";
import { Button, Select, Skeleton, Toggle, toast } from "../../../ui";
import { CardGroup, Group, SettingRow } from "../parts";
import { useSetSettings } from "./Mods";

export function OnlineSection() {
  const crucible = useCrucibleOn();
  const nexus = useNexusEnabled();
  return (
    <>
      <UpdatesGroup />
      {nexus && <CardGroup id="nexus" items={["nexus", "nexus.handler"]}><NexusCard /></CardGroup>}
      <CommunityGroup />
      {crucible && <SendsGroup />}
    </>
  );
}

/** Launcher updates (direct builds: automatic checks, channel, the state and Check now, a test build's way back), or the
 *  Nexus build's two lines (components/UpdateSettings.tsx has the pieces). */
function UpdatesGroup() {
  const dist = useDistribution();
  if (dist === "nexus") return (
    <Group id="updates" items={["updates.nexus"]}>
      <div className="k-st-row" data-setting="updates.nexus">
        <div className="k-st-meta">
          <span className="k-hint" data-testid="nexus-versions">{rich("updateSettings.newVersions", { link: <NexusVersionsLink /> })}</span>
          <span className="k-hint" data-testid="nexus-beta">{rich("updateSettings.nexusBeta", { link: <NexusTestBuildsLink /> })}</span>
        </div>
      </div>
    </Group>
  );
  if (dist === "direct") return <SelfUpdates />;
  return <Group id="updates" items={[]}><Skeleton /></Group>;
}

function SelfUpdates() {
  const t = useT();
  const qc = useQueryClient();
  const s = useUpdate();
  const settings = useSettings();
  const check = useCheckNow();
  const save = useMutation({
    mutationFn: (patch: Partial<LauncherSettings>) => saveSettings(qc, () => patch),
    onSuccess: (v) => { if (v) api.updateStatus().then((x) => useUpdateStore.getState().setStatus(x)).catch(() => {}); },
  });
  const items = ["updates.auto", "updates.channel", "updates.state"];
  if (!s || !settings.data) return <Group id="updates" items={items}><Skeleton /></Group>;
  const v = settings.data;
  const busy = check.busy || s.phase === "checking" || s.phase === "downloading";
  const show = useChannelSwitch.getState().show;
  // a switch (the release on a test build, the beta on a release build) asks first; anything else only follows the channel
  const setChannel = (ch: string) => {
    const target = ch as UpdateChannel;
    if (switchKind(s, target)) { show(target); return; }
    save.mutate({ updateChannel: target }, { onSuccess: (saved) => { if (saved && saved.updateCheck !== false) check.run(); } });
  };
  const undo = () => api.updateSkip(null).catch(() => toast.error({ code: "internal", message: t("updateSettings.undoFailed") }));
  return (
    <Group id="updates" items={items} hint={<span className="k-mono">{s.current}</span>}>
      <SettingRow id="updates.auto" htmlFor="st-upd-auto"
        control={<Toggle id="st-upd-auto" checked={v.updateCheck !== false} onChange={(on) => save.mutate({ updateCheck: on })} />} />
      <SettingRow id="updates.channel" htmlFor="st-upd-channel"
        hint={s.buildChannel !== "beta" && s.channel === "beta" ? <span data-testid="beta-follow">{t("updateSettings.followBeta")}</span> : undefined}
        control={<Select id="st-upd-channel" value={s.channel} onChange={setChannel} style={{ width: 200 }} options={channelOptions(s.channel)} />} />
      <SettingRow id="updates.state"
        hint={<span role="status">
          {updateWords(s)}
          {s.phase === "skipped" && <>{" · "}<button type="button" className="k-link" onClick={undo}>{t("common.undo")}</button></>}
        </span>}
        control={<Button size="s" loading={busy} onClick={check.run}>{t("updateSettings.checkNow")}</Button>} />
      {s.buildChannel === "beta" && (
        <div className="k-st-row" data-testid="beta-build">
          <div className="k-st-meta"><span className="k-st-hint">{t("updateSettings.testBuild", { beta: s.beta ?? "", current: s.current })}</span></div>
          <div className="k-st-ctl"><Button size="s" onClick={() => show(RELEASE_CHANNEL)}>{t("updateSettings.switchBack")}</Button></div>
        </div>
      )}
    </Group>
  );
}

/** Show the Community page (the launcher's Discord page: community/). */
function CommunityGroup() {
  const qc = useQueryClient();
  const s = useCommunitySettings();
  return (
    <Group id="community" items={["community.shown"]}>
      <SettingRow id="community.shown" htmlFor="st-community" testId="community-shown"
        control={<Toggle id="st-community" checked={s.data?.shown !== false} disabled={!s.data}
          onChange={(on) => { setCommunity(qc, { shown: on }).catch((e) => toast.error(e)); }} />} />
    </Group>
  );
}

/** (dev.30) Crucible's anonymous install counts; only where this build has Crucible. */
function SendsGroup() {
  const s = useSettings();
  const set = useSetSettings();
  return (
    <Group id="sends" items={["crucibleStats"]}>
      <SettingRow id="crucibleStats" htmlFor="st-crucible-stats"
        control={<Toggle id="st-crucible-stats" checked={s.data?.crucibleStats !== false} disabled={!s.data} onChange={(on) => set({ crucibleStats: on })} />} />
    </Group>
  );
}
