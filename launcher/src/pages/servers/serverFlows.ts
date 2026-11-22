// The server actions as flows (serverActions.ts), for the Servers list and the new-server wizard: the same steps, words
// and results as the server head's (ServerLayout.tsx builds its own; these are the same specs, so a flow started here
// reads the same in the server's activity panel).
import type { QueryClient } from "@tanstack/react-query";
import * as api from "../../api/commands";
import { installNote } from "../../ui";
import { notifyInstall } from "../../shell/notices";
import { t } from "../../i18n";
import { cancelStep, runFlow, useServerFlows, type FlowSpec, type StepSpec } from "./serverActions";
import { sk, type ServerSpecX } from "./serverApi";

export const stepStop = (id: string): StepSpec => ({ key: "stop", label: t("servers.activity.stepStop"), run: () => api.serverStop(id), phase: "stopping" });
export const stepStart = (id: string, again = false): StepSpec => ({
  key: "start", label: again ? t("servers.activity.stepStartAgain") : t("servers.activity.stepStart"), run: () => api.serverStart(id), phase: "starting", cancel: "stopWaiting",
});
const stepDeploy = (id: string, version: string): StepSpec => ({
  key: "deploy", label: t("servers.activity.stepDeploy", { version }), run: () => api.serverDeploy(id, version), result: true, phase: "updating", cancel: "cancel",
});

export const startFlow = (id: string, name: string): FlowSpec => ({ kind: "start", title: t("servers.activity.startTitle", { name }), steps: [stepStart(id)], done: t("servers.head.runningToast") });
export const stopFlow = (id: string, name: string): FlowSpec => ({ kind: "stop", title: t("servers.activity.stopTitle", { name }), steps: [stepStop(id)], done: t("servers.head.stoppedToast") });
export const restartFlow = (id: string, name: string): FlowSpec => ({
  kind: "restart", title: t("servers.activity.restartTitle", { name }),
  steps: [{ key: "restart", label: t("servers.activity.stepRestart"), run: () => api.serverRestart(id), phase: "stopping" }], done: t("servers.head.restarted"),
});
export const backupFlow = (id: string, name: string): FlowSpec => ({
  kind: "backup", title: t("servers.list.backupTitle", { name }),
  steps: [{ key: "backup", label: t("servers.list.stepBackup"), run: () => api.serverBackupCreate(id), cancel: "cancel" }], done: t("servers.backups.made"),
});

export interface KilnFlowOptions {
  id: string;
  name: string;
  version: string;
  /** the server runs (a process): it is stopped first and started again */
  running: boolean;
  /** start a stopped server afterwards */
  startAfter?: boolean;
  /** the Kiln in its folder now (an update, not an install) */
  from?: string | null;
}

/** Install or update Kiln: stop a running server, install the bundle, start it again (or a stopped one when asked). */
export function kilnFlow(o: KilnFlowOptions): FlowSpec<api.InstallResult> {
  const again = o.running || !!o.startAfter;
  return {
    kind: "deploy",
    title: o.from ? t("servers.activity.updateKilnTitle", { version: o.version, name: o.name }) : t("servers.activity.deployTitle", { version: o.version, name: o.name }),
    steps: [...(o.running ? [stepStop(o.id)] : []), stepDeploy(o.id, o.version), ...(again ? [stepStart(o.id, o.running)] : [])],
    done: t("servers.head.kilnInstalled", { version: o.version }), detail: installNote, after: notifyInstall,
    meta: { from: o.from ?? null },
  };
}

/** Stop, as the head does: a start still waiting for its world stops waiting and the stop takes over. */
export function stopServer(qc: QueryClient, id: string, name: string): Promise<boolean> {
  const f = useServerFlows.getState().flows[id];
  const starting = f?.state === "running" && f.kind === "start";
  if (starting) void cancelStep(id).catch(() => {});
  return runFlow(qc, id, stopFlow(id, name), { supersede: starting });
}

export interface CreatePlan {
  spec: ServerSpecX;
  /** install this Kiln bundle when the files are in */
  kiln: string | null;
  /** start the server at the end */
  start: boolean;
}

/** The new-server wizard's flow: install the server (server_create), then Kiln and the start when chosen. The server
 *  shows in the list as soon as its instance file is written (the task's first step), so the list is read again then. */
export function createFlow(qc: QueryClient, plan: CreatePlan): FlowSpec {
  const { spec } = plan;
  const id = spec.id;
  return {
    kind: "create",
    title: t("servers.wizard.flowTitle", { name: spec.name }),
    steps: [
      { key: "create", label: spec.reflinkFrom ? (spec.skipSteamcmd ? t("servers.wizard.stepCopyOnly") : t("servers.wizard.stepCopy")) : t("servers.wizard.stepCreate"), cancel: "cancel",
        run: async () => {
          const task = await api.serverCreate(spec);
          setTimeout(() => qc.invalidateQueries({ queryKey: sk.list }), 400);
          return task;
        } },
      ...(plan.kiln ? [stepDeploy(id, plan.kiln)] : []),
      ...(plan.start ? [stepStart(id)] : []),
    ],
    done: t("servers.list.created", { name: spec.name }),
  };
}
