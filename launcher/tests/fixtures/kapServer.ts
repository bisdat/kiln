// A fake KAP/1-over-WebSocket server for the admin pages in a plain page (src/admin/harness): Playwright's
// routeWebSocket in front of the same in-memory Kiln.Admin as the launcher's browser mock (src/admin/mock/world.ts, the
// committed synthetic fixture). Used by admin-ui-web.spec.ts and admin-ui-csp.spec.ts.
import type { Page } from "@playwright/test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { MockWorld, OpError, type Fixture } from "../../src/admin/mock/world";

const FIXTURE = JSON.parse(readFileSync(join(import.meta.dirname, "..", "..", "src", "admin", "mock", "fixtures", "synthetic.json"), "utf8")) as Fixture;

export type Frame = { t: string; [k: string]: unknown };

/** A KAP/1-over-WebSocket server per socket: hello, ready, then the mock world's ops and a state push every second.
 *  `/kap/offline` closes at once, as when the server is not running. */
export async function kapServer(page: Page, setup: (world: MockWorld) => void = () => undefined) {
  const frames: Frame[] = [];
  const urls: string[] = [];
  await page.routeWebSocket(/\/kap\//, (ws) => {
    urls.push(ws.url());
    if (ws.url().endsWith("/offline")) {
      ws.close({ code: 1011, reason: "the server is not running" });
      return;
    }
    const world = new MockWorld(FIXTURE);
    setup(world);
    const send = (f: Frame) => ws.send(JSON.stringify(f));
    let seq = 0;
    let timer: ReturnType<typeof setInterval> | null = null;
    const push = () => send({ t: "push", topic: "state", seq: ++seq, data: world.state() as unknown as Frame });
    send({ t: "hello", proto: "kap/1", kiln: world.kiln, admin: "0.2.0", server: FIXTURE.server as unknown as Frame });
    send({ t: "ready", session: "k1", ops: world.ops, topics: ["state", "log"], limits: FIXTURE.limits as unknown as Frame });
    ws.onMessage((m) => {
      const f = JSON.parse(String(m)) as Frame;
      frames.push(f);
      if (f.t === "req") {
        const op = String(f.op);
        if (!world.ops.includes(op)) return send({ t: "res", id: f.id, ok: false, op, error: `unknown op ${op}`, code: "unknown_op" });
        try {
          send({ t: "res", id: f.id, ok: true, op, ms: 1, data: world.run(op, { ...(f.args as object), op }) as unknown as Frame });
        } catch (e) {
          send({ t: "res", id: f.id, ok: false, op, ms: 0, error: e instanceof Error ? e.message : String(e), code: e instanceof OpError ? "op_failed" : "internal" });
        }
      } else if (f.t === "sub") {
        send({ t: "res", id: f.id, ok: true, op: "sub" });
        if (f.topic === "state" && !timer) {
          push();
          timer = setInterval(() => { world.tick(); push(); }, 1000);
        }
      } else if (f.t === "unsub") send({ t: "res", id: f.id, ok: true, op: "unsub" });
      else if (f.t === "ping") send({ t: "pong", id: f.id, ts: f.ts });
    });
    ws.onClose(() => { if (timer) clearInterval(timer); });
  });
  return { frames, urls };
}
