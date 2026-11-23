// The WebSocket admin transport (src/api/adminWs.ts) against a fake KAP/1-over-WebSocket server (Bun.serve): the
// host page's socket is already signed in, so the server opens with `hello` (optional) and `ready`, then the frames of
// admin-protocol.md sections 4-6 travel as one JSON object per message. It behaves like the launcher's Rust AdminHub:
// one session per server, reconnect with backoff, re-subscribe, closed{retry}/ready events, pings both ways.
import { afterAll, afterEach, beforeAll, describe, expect, test } from "bun:test";
import type { ServerWebSocket } from "bun";
import { createWebSocketTransport } from "../src/api/adminWs";
import type { AdminHubEvent } from "../src/api/admin";

type Frame = Record<string, unknown> & { t: string };
interface Fake {
  url: string;
  frames: Frame[];
  sockets: Set<ServerWebSocket<unknown>>;
  opens: number;
  /** what the server does when a socket opens / a frame comes */
  onOpen: (ws: ServerWebSocket<unknown>) => void;
  onFrame: (ws: ServerWebSocket<unknown>, f: Frame) => void;
  stop: () => void;
}

const send = (ws: ServerWebSocket<unknown>, f: Frame) => ws.send(JSON.stringify(f));
const HELLO = { t: "hello", proto: "kap/1", kiln: "0.2.0-dev.30.9", admin: "0.2.0", server: { id: "srv-1", name: "Ember Isles" } };
const READY = { t: "ready", session: "k1", ops: ["players", "heal", "server_restart", "mod_test"], topics: ["state", "log", "metrics"], limits: { maxFrame: 16 << 20, maxClientFrame: 1 << 20, inflight: 64, pingS: 15, idleS: 45 }, restart: { supported: "supported", via: "kilnctl", code: 75 } };

function fakeServer(): Fake {
  const fake: Fake = {
    url: "", frames: [], sockets: new Set(), opens: 0,
    onOpen: (ws) => { send(ws, HELLO); send(ws, READY); },
    onFrame: (ws, f) => {
      if (f.t === "req") {
        const op = f.op as string;
        if (op === "players") send(ws, { t: "res", id: f.id, ok: true, op, ms: 1, data: [{ entity: 1, name: "Mira" }] });
        else if (op === "heal") send(ws, { t: "res", id: f.id, ok: false, op, ms: 0, error: "entity must be an entity id", code: "op_failed" });
        else if (op === "busy_op") send(ws, { t: "res", id: f.id, ok: false, op, error: "too many", code: "busy" });
        else if (op === "slow") { /* never answers */ }
        else send(ws, { t: "res", id: f.id, ok: true, op, data: { echo: f.args ?? null } });
      } else if (f.t === "sub") {
        send(ws, { t: "res", id: f.id, ok: true, op: "sub" });
        if (f.topic === "state") send(ws, { t: "push", topic: "state", seq: 1, data: { clock: 1, kiln: "x", ready: true } });
      } else if (f.t === "unsub") send(ws, { t: "res", id: f.id, ok: true, op: "unsub" });
      else if (f.t === "ping") send(ws, { t: "pong", id: f.id, ts: f.ts });
    },
    stop: () => server.stop(true),
  };
  const server = Bun.serve({
    port: 0,
    fetch(req, srv) {
      const u = new URL(req.url);
      if (u.searchParams.get("ticket") === "expired") return new Response("sign in", { status: 401 });
      return srv.upgrade(req, { data: null }) ? undefined : new Response("websocket only", { status: 400 });
    },
    websocket: {
      open(ws) { fake.opens++; fake.sockets.add(ws); fake.onOpen(ws); },
      message(ws, m) { const f = JSON.parse(String(m)) as Frame; fake.frames.push(f); fake.onFrame(ws, f); },
      close(ws) { fake.sockets.delete(ws); },
    },
  });
  fake.url = `ws://127.0.0.1:${server.port}/kap`;
  return fake;
}

const wait = (ms: number) => new Promise((r) => setTimeout(r, ms));
async function until(fn: () => boolean, ms = 3000) {
  const end = Date.now() + ms;
  while (!fn()) {
    if (Date.now() > end) throw new Error("timed out waiting");
    await wait(10);
  }
}

let fake: Fake;
const transports: { dispose?: () => void; disconnectAll?: () => Promise<void> }[] = [];
function make(opts: Partial<Parameters<typeof createWebSocketTransport>[0]> = {}) {
  const t = createWebSocketTransport({ url: (id) => `${fake.url}?server=${encodeURIComponent(id)}`, backoffMs: [50, 200], ...opts });
  transports.push(t as never);
  const events: AdminHubEvent[] = [];
  void t.listen((e) => events.push(e));
  return { t, events };
}
afterEach(async () => {
  for (const t of transports.splice(0)) await (t as { disconnectAll?: () => Promise<void> }).disconnectAll?.();
  fake?.stop();
});

describe("connecting", () => {
  test("hello and ready make the session: ops, topics, the server's name, restart support; a ready event", async () => {
    fake = fakeServer();
    const urls: string[] = [];
    const { t, events } = make({ url: (id) => { urls.push(id); return fake.url; } });
    const s = await t.connect("lab", "auto");
    expect(urls).toEqual(["lab"]);
    expect(s).toMatchObject({ serverId: "lab", transport: "ws", ops: READY.ops, topics: READY.topics, kiln: "0.2.0-dev.30.9", server: { id: "srv-1", name: "Ember Isles" }, restart: READY.restart });
    expect(s.session).toMatch(/^ws-lab-\d+$/);
    expect(events).toEqual([{ session: s.session, serverId: "lab", event: { kind: "ready", transport: "ws", ops: READY.ops, topics: READY.topics, restart: READY.restart } }]);
    // one session per server: connect again returns it
    expect((await t.connect("lab", "tcp")).session).toBe(s.session);
    expect(fake.opens).toBe(1);
    // the browser never authenticates: no auth frame, no token
    expect(fake.frames.some((f) => f.t === "auth")).toBe(false);
  });

  test("ready without hello is enough", async () => {
    fake = fakeServer();
    fake.onOpen = (ws) => send(ws, READY);
    const { t } = make();
    const s = await t.connect("lab", "auto");
    expect(s).toMatchObject({ kiln: null, server: {} });
  });

  test("an error frame before ready refuses the connect (auth errors as auth_failed)", async () => {
    fake = fakeServer();
    fake.onOpen = (ws) => { send(ws, { t: "error", code: "auth_failed", message: "not signed in" }); ws.close(); };
    const { t } = make();
    await expect(t.connect("lab", "auto")).rejects.toMatchObject({ code: "auth_failed" });
    fake.onOpen = (ws) => { send(ws, { t: "error", code: "busy", message: "4 connections" }); ws.close(); };
    await expect(t.connect("lab", "auto")).rejects.toMatchObject({ code: "busy" });
  });

  test("a server that never says ready, a refused upgrade and nothing listening fail the connect", async () => {
    fake = fakeServer();
    fake.onOpen = (ws) => send(ws, HELLO);
    const { t } = make({ handshakeMs: 200 });
    await expect(t.connect("lab", "auto")).rejects.toMatchObject({ code: "admin_unavailable" });
    const r = make({ url: () => `${fake.url}?ticket=expired` });
    await expect(r.t.connect("lab", "auto")).rejects.toMatchObject({ code: "admin_unavailable" });
    const dead = make({ url: () => "ws://127.0.0.1:9/kap" });
    await expect(dead.t.connect("lab", "auto")).rejects.toMatchObject({ code: "admin_unavailable" });
  });

  test("the url callback may refuse (signed out): auth_failed", async () => {
    fake = fakeServer();
    const { t } = make({ url: () => { throw { code: "auth_failed", message: "signed out" }; } });
    await expect(t.connect("lab", "auto")).rejects.toMatchObject({ code: "auth_failed", message: "signed out" });
  });

  test("a socket the browser refuses to make: admin_unavailable, and the message shows the URL without its ticket", async () => {
    fake = fakeServer();
    // as Chrome words it: the URL, query and all
    class Refused { constructor(url: string) { throw new SyntaxError(`Failed to construct 'WebSocket': The URL '${url}' is invalid.`); } }
    const { t } = make({ url: () => "ws://bad host/kap/lab?ticket=s3cret", WebSocket: Refused as never });
    const err = (await t.connect("lab", "auto").catch((e: unknown) => e)) as { code: string; message: string };
    expect(err.code).toBe("admin_unavailable");
    expect(err.message).toContain("'ws://bad host/kap/lab'");
    expect(err.message).not.toContain("s3cret");
  });

  test("limits that are not numbers keep their defaults: requests still go, the 1 MiB cap still holds", async () => {
    fake = fakeServer();
    fake.onOpen = (ws) => send(ws, { ...READY, limits: { maxFrame: "big", maxClientFrame: null, inflight: -1, pingS: "x", idleS: null } });
    const { t } = make();
    const s = await t.connect("lab", "auto");
    expect(await t.call(s.session, "echo", { text: "hi" })).toEqual({ echo: { text: "hi" } });
    await expect(t.call(s.session, "echo", { text: "x".repeat((1 << 20) + 1) })).rejects.toMatchObject({ code: "invalid_argument" });
    expect(fake.frames.filter((f) => f.t === "req")).toHaveLength(1);
  });
});

describe("requests", () => {
  beforeAll(() => undefined);
  test("req/res by id: data, op errors as KilnError with the KAP code in detail", async () => {
    fake = fakeServer();
    const { t } = make();
    const s = await t.connect("lab", "auto");
    expect(await t.call(s.session, "players", {})).toEqual([{ entity: 1, name: "Mira" }]);
    const req = fake.frames.find((f) => f.t === "req")!;
    expect(req).toEqual({ t: "req", id: expect.any(Number), op: "players", args: {} });
    await expect(t.call(s.session, "heal", { entity: "x" })).rejects.toMatchObject({ code: "op_failed", message: "entity must be an entity id", detail: { op: "heal", transport: "ws", kapCode: "op_failed" } });
    await expect(t.call(s.session, "busy_op", {})).rejects.toMatchObject({ code: "busy" });
    expect(await t.call(s.session, "mod_test", { id: "a" })).toEqual({ echo: { id: "a" } });
  });

  test("answers out of order reach their own callers", async () => {
    fake = fakeServer();
    const held: Frame[] = [];
    fake.onFrame = (ws, f) => {
      if (f.t !== "req") return;
      held.push(f);
      if (held.length === 2) for (const h of [...held].reverse()) send(ws, { t: "res", id: h.id, ok: true, op: h.op, data: h.op });
    };
    const { t } = make();
    const s = await t.connect("lab", "auto");
    const [a, b] = await Promise.all([t.call(s.session, "players", {}), t.call(s.session, "heal", {})]);
    expect([a, b]).toEqual(["players", "heal"]);
  });

  test("an op that does not answer in time: timeout", async () => {
    fake = fakeServer();
    const { t } = make({ callTimeoutMs: () => 150 });
    const s = await t.connect("lab", "auto");
    await expect(t.call(s.session, "slow", {})).rejects.toMatchObject({ code: "timeout" });
  });

  test("a request larger than the server takes is refused without sending it", async () => {
    fake = fakeServer();
    fake.onOpen = (ws) => send(ws, { ...READY, limits: { ...READY.limits, maxClientFrame: 200 } });
    const { t } = make();
    const s = await t.connect("lab", "auto");
    await expect(t.call(s.session, "players", { text: "x".repeat(500) })).rejects.toMatchObject({ code: "invalid_argument" });
    expect(fake.frames.filter((f) => f.t === "req")).toEqual([]);
  });

  test("an unknown session: not_found", async () => {
    fake = fakeServer();
    const { t } = make();
    await expect(t.call("ws-nope-1", "players", {})).rejects.toMatchObject({ code: "not_found" });
    await expect(t.subscribe("ws-nope-1", "state")).rejects.toMatchObject({ code: "not_found" });
  });
});

describe("pushes and pings", () => {
  test("subscribe sends sub; pushes arrive as push events; a topic the server lacks is refused", async () => {
    fake = fakeServer();
    const { t, events } = make();
    const s = await t.connect("lab", "auto");
    await t.subscribe(s.session, "state");
    await until(() => events.some((e) => e.event.kind === "push"));
    expect(fake.frames.find((f) => f.t === "sub")).toEqual({ t: "sub", id: expect.any(Number), topic: "state" });
    expect(events.find((e) => e.event.kind === "push")).toEqual({ session: s.session, serverId: "lab", event: { kind: "push", topic: "state", seq: 1, data: { clock: 1, kiln: "x", ready: true } } });
    await expect(t.subscribe(s.session, "nope")).rejects.toMatchObject({ code: "invalid_argument" });
    await t.unsubscribe(s.session, "state");
    expect(fake.frames.some((f) => f.t === "unsub" && f.topic === "state")).toBe(true);
  });

  test("the server's ping gets a pong with its id and ts; an error frame becomes an error event", async () => {
    fake = fakeServer();
    const { t, events } = make();
    const s = await t.connect("lab", "auto");
    const ws = [...fake.sockets][0];
    send(ws, { t: "ping", id: 99, ts: 1234 });
    send(ws, { t: "error", code: "bad_frame", message: "unknown t" });
    send(ws, { t: "future_frame", x: 1 });  // a newer frame type: ignored
    await until(() => events.some((e) => e.event.kind === "error"));
    expect(fake.frames).toContainEqual({ t: "pong", id: 99, ts: 1234 });
    expect(events.find((e) => e.event.kind === "error")).toEqual({ session: s.session, serverId: "lab", event: { kind: "error", message: "bad_frame: unknown t" } });
  });

  test("the client pings after pingS without sending, and drops a server silent for idleS", async () => {
    fake = fakeServer();
    fake.onOpen = (ws) => send(ws, { ...READY, limits: { ...READY.limits, pingS: 1, idleS: 2 } });
    let answerPings = true;
    const base = fake.onFrame;
    fake.onFrame = (ws, f) => { if (f.t === "ping" && !answerPings) return; base(ws, f); };
    const { t, events } = make();
    const s = await t.connect("lab", "auto");
    await until(() => fake.frames.some((f) => f.t === "ping"), 2500);
    answerPings = false;
    await until(() => events.some((e) => e.event.kind === "closed"), 4000);
    expect(events.find((e) => e.event.kind === "closed")).toMatchObject({ session: s.session, event: { kind: "closed", retry: true, reason: expect.stringContaining("silent") } });
  });
});

describe("reconnecting like the launcher's hub", () => {
  test("a dropped connection: closed{retry}, a new socket, the topics again, ready; calls meanwhile say reconnecting", async () => {
    fake = fakeServer();
    const { t, events } = make();
    const s = await t.connect("lab", "auto");
    await t.subscribe(s.session, "state");
    let open = true;
    fake.onOpen = (ws) => { if (open) { send(ws, HELLO); send(ws, READY); } };
    open = false;
    for (const ws of fake.sockets) ws.close(1001, "server stopping");
    await until(() => events.some((e) => e.event.kind === "closed"));
    expect(events.find((e) => e.event.kind === "closed")!.event).toEqual({ kind: "closed", reason: expect.any(String), retry: true });
    await expect(t.call(s.session, "players", {})).rejects.toMatchObject({ code: "admin_unavailable" });
    open = true;
    const subsBefore = fake.frames.filter((f) => f.t === "sub").length;
    await until(() => events.filter((e) => e.event.kind === "ready").length === 2);
    expect(fake.frames.filter((f) => f.t === "sub").length).toBe(subsBefore + 1);
    // closed, then ready, then the topic's pushes (as the launcher's hub orders them)
    await until(() => events.filter((e) => e.event.kind === "push").length === 2);
    const after = events.slice(events.findIndex((e) => e.event.kind === "closed")).map((e) => e.event.kind);
    expect(after).toEqual(["closed", "ready", "push"]);
    expect(events.find((e, i) => i > 1 && e.event.kind === "ready")).toMatchObject({ session: s.session, event: { kind: "ready", transport: "ws" } });
    expect(await t.call(s.session, "players", {})).toEqual([{ entity: 1, name: "Mira" }]);
  });

  test("bye from the server is a close with its reason", async () => {
    fake = fakeServer();
    const { t, events } = make();
    const s = await t.connect("lab", "auto");
    for (const ws of fake.sockets) send(ws, { t: "bye", reason: "unload" });
    await until(() => events.some((e) => e.event.kind === "closed"));
    expect(events.find((e) => e.event.kind === "closed")).toMatchObject({ session: s.session, event: { reason: "bye: unload", retry: true } });
  });

  test("closed with 4401 (signed out) or an auth error at the reconnect is final: closed{retry: false}", async () => {
    fake = fakeServer();
    const { t, events } = make();
    const s = await t.connect("lab", "auto");
    for (const ws of fake.sockets) ws.close(4401, "signed out");
    await until(() => events.some((e) => e.event.kind === "closed"));
    expect(events.find((e) => e.event.kind === "closed")!.event).toEqual({ kind: "closed", reason: "signed out", retry: false });
    await wait(300);
    expect(fake.opens).toBe(1);
    await expect(t.call(s.session, "players", {})).rejects.toMatchObject({ code: "not_found" });
    // a new connect after that is a new session
    const again = await t.connect("lab", "auto");
    expect(again.session).not.toBe(s.session);
  });

  test("disconnect: bye, the socket closes, closed{retry: false}, the session is gone", async () => {
    fake = fakeServer();
    const { t, events } = make();
    const s = await t.connect("lab", "auto");
    await t.disconnect(s.session);
    await until(() => fake.sockets.size === 0);
    expect(fake.frames.at(-1)).toEqual({ t: "bye", reason: "disconnect" });
    expect(events.at(-1)).toEqual({ session: s.session, serverId: "lab", event: { kind: "closed", reason: "disconnected", retry: false } });
    await expect(t.call(s.session, "players", {})).rejects.toMatchObject({ code: "not_found" });
    await expect(t.disconnect(s.session)).rejects.toMatchObject({ code: "not_found" });
  });
});

describe("the web transport never reaches Tauri", () => {
  const g = globalThis as unknown as { window?: unknown };
  const invoked: string[] = [];
  let had = false;
  beforeAll(() => {
    had = "window" in g;
    // as if the web panel were opened inside the launcher's webview: Tauri's IPC is right there
    if (!had) g.window = Object.assign(Object.create(globalThis), { __TAURI_INTERNALS__: { invoke: async (cmd: string) => { invoked.push(cmd); return null; }, transformCallback: () => 1 } });
  });
  afterAll(() => { if (!had) delete g.window; });

  test("server_restart over the WebSocket transport is a KAP req on the socket, never a Tauri command", async () => {
    fake = fakeServer();
    const { t } = make();
    const s = await t.connect("lab", "auto");
    expect(await t.call(s.session, "server_restart", { seconds: 60 })).toEqual({ echo: { seconds: 60 } });
    expect(fake.frames.filter((f) => f.t === "req")).toEqual([{ t: "req", id: expect.any(Number), op: "server_restart", args: { seconds: 60 } }]);
    expect(invoked).toEqual([]);
  });

  test("through the hub and kapOps as well, and the transport offers no launcher kinds", async () => {
    fake = fakeServer();
    const { acquireAdmin, callAdmin, configureAdmin, kapOps, releaseAdmin, adminHost } = await import("../src/api/admin");
    const { t } = make();
    configureAdmin({ transport: t });
    acquireAdmin("lab");
    await until(() => fake.frames.some((f) => f.t === "sub"));
    await kapOps((op, args) => callAdmin("lab", op, args)).serverRestart({ seconds: 5 });
    expect(fake.frames.filter((f) => f.t === "req").map((f) => f.op)).toEqual(["server_restart"]);
    expect(invoked).toEqual([]);
    expect(t.kinds).toEqual([]);
    expect(adminHost().restartManagedServer).toBeUndefined();
    releaseAdmin("lab");
    configureAdmin({ transport: null });
  });
});
