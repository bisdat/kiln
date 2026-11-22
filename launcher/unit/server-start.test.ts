import { expect, test } from "bun:test";
import { onKilnEvent, serverRestart, serverStatus, taskGet, type Task } from "../src/api/commands";

test("the mock restart publishes stopping and the five start stages", async () => {
  const stages: string[] = [];
  let finish!: (task: Task) => void;
  const done = new Promise<Task>((resolve) => { finish = resolve; });
  const unlisten = await onKilnEvent("kiln://task", (task) => {
    if (task.kind !== "server_restart") return;
    if (task.state !== "running") finish(task);
    else if (task.stage) stages.push(task.stage);
  });

  try {
    const id = await serverRestart("lab");
    const finished = await done;
    expect(stages).toEqual(["stopping", "preparing", "wine", "process", "world", "running"]);
    expect(finished).toMatchObject({ id, stage: "running", state: "done", result: { state: "running", lastStartS: 1 } });
    expect(await taskGet(id)).toMatchObject({ stage: "running", state: "done" });
    expect(await serverStatus("lab")).toMatchObject({ state: "running", lastStartS: 1 });
  } finally {
    unlisten();
  }
}, 10_000);
