// The admin transport for web pages (Kilnwarden, through @kiln/admin-ui): KAP/1 frames as JSON over a WebSocket,
// one frame per message (admin-protocol.md section 12). The socket is already signed in: the host page gives its URL
// (a cookie or a one-time ticket in it), the server side holds the admin token and has done the KAP handshake with
// Kiln.Admin, so the browser never authenticates and never sees the token. The server opens with `hello`
// (optional: kiln, admin, server) and `ready` (ops, topics, limits, restart); then req/res, sub/unsub/push, ping/pong,
// error and bye as over TCP.
//
// It behaves like the launcher's Rust AdminHub (kiln_manage::admin::hub), so the hub in ./admin.ts sees the same
// events over either transport: one session per server, stable across reconnects; a dropped connection is
// `closed {retry: true}`, reconnects with backoff (1, 2, 4 ... 30 s), subscribes its topics again and says `ready`; an
// auth failure (an `error` frame, or close code 4401/4403 from the host) is final, `closed {retry: false}`. It pings
// after `limits.pingS` without sending and drops a server silent for `limits.idleS`.
//
// Tauri-free by design: nothing here (or in what it imports) can reach the launcher's commands. Tests:
// unit-tests/admin-ws-transport.test.ts (a fake server), unit-tests/admin-ui-package.test.ts (what it reaches).
import { t } from "../i18n";
import type { AdminEvent, AdminHubEvent, AdminSession, AdminTransport, KapErrorCode, KapLimits, KapRestart } from "./admin";
import { isKilnError, type ErrorCode, type Json, type KilnError, type ServerId } from "./shared";

/** The WebSocket class (the browser's by default; tests and Node may pass their own). */
export interface WebSocketLike {
  readonly readyState: number;
  binaryType: string;
  send(data: string): void;
  close(code?: number, reason?: string): void;
  addEventListener(type: "open" | "message" | "error" | "close", listener: (ev: never) => void): void;
}
export type WebSocketClass = new (url: string) => WebSocketLike;

export interface WebSocketTransportOptions {
  /** The signed-in socket's URL for a server: a string, or a function called at every (re)connect (so the host can hand
   * out a fresh ticket). A function that throws a KilnError with code `auth_failed` ends the session (signed out). */
  url: string | ((serverId: ServerId) => string | Promise<string>);
  WebSocket?: WebSocketClass;
  /** Reconnect backoff, first and largest delay (default 1 s, 30 s). */
  backoffMs?: readonly [number, number];
  /** How long a connect waits for `ready` (default 10 s). */
  handshakeMs?: number;
  /** How long a request may take (default: the op's budget + 10 s, as the launcher). */
  callTimeoutMs?: (op: string) => number;
}

/** The transport, plus closing every session (page unload, tests). */
export interface WebSocketAdminTransport extends AdminTransport {
  disconnectAll(): Promise<void>;
}

type Frame = { t: string; [k: string]: unknown };
const DEFAULT_LIMITS: KapLimits = { maxFrame: 16 << 20, maxClientFrame: 1 << 20, inflight: 64, pingS: 15, idleS: 45 };
/** Close codes the host's endpoint uses for "not signed in" and "not allowed": final, never retried. */
const AUTH_CLOSE = new Set([4401, 4403]);

/** An op's budget in seconds (admin-protocol.md section 4; Rust protocol::op_budget). */
export function opBudgetS(op: string): number {
  if (op === "mod_test" || op === "update_stage") return 180;
  if (op === "backup_create") return 90;
  if (op === "save_now") return 60;
  if (op.startsWith("mod_")) return 45;
  return 30;
}

/** A KAP error code as the launcher's error code (Rust protocol::error_code). */
function errorCode(code: unknown): ErrorCode {
  switch (code as KapErrorCode) {
    case "busy": return "busy";
    case "timeout": return "timeout";
    case "auth_failed": case "auth_required": return "auth_failed";
    case "shutting_down": return "admin_unavailable";
    case "unknown_topic": case "bad_frame": return "invalid_argument";
    case "version": return "unsupported";
    case "internal": return "internal";
    default: return "op_failed";
  }
}

const kerr = (code: ErrorCode, message: string, detail?: unknown): KilnError => (detail === undefined ? { code, message } : { code, message, detail });
const sanitize = (s: string) => s.replace(/[^A-Za-z0-9_-]/g, "_").slice(0, 48);
const isObj = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

interface Waiter {
  resolve: (f: Frame) => void;
  reject: (e: KilnError) => void;
  timer: ReturnType<typeof setTimeout> | null;
}

/** One WebSocket: the handshake, then frames by id, pushes and the keepalive. */
class Link {
  hello: Frame | null = null;
  ready!: Frame;
  limits: KapLimits = DEFAULT_LIMITS;
  closed = false;
  private next = 1;
  private waiting = new Map<number, Waiter>();
  private sentAt = Date.now();
  private receivedAt = Date.now();
  private keepalive: ReturnType<typeof setInterval> | null = null;
  onPush: (topic: string, seq: number, data: Json) => void = () => undefined;
  onError: (message: string) => void = () => undefined;
  onClose: (reason: string, final: boolean) => void = () => undefined;

  private constructor(private ws: WebSocketLike) {}

  /** Opens the socket and waits for `ready`. */
  static open(Ws: WebSocketClass, url: string, handshakeMs: number): Promise<Link> {
    return new Promise((resolve, reject) => {
      let ws: WebSocketLike;
      try {
        ws = new Ws(url);
      } catch (e) {
        // a browser's message may quote the URL, and with it a sign-in ticket in its query: the page shows it without one
        const reason = (e instanceof Error ? e.message : String(e)).split(url).join(url.replace(/\?.*$/, ""));
        reject(kerr("admin_unavailable", t("api.adminWs.unreachable", { reason })));
        return;
      }
      const link = new Link(ws);
      let stage: "handshake" | "open" | "done" = "handshake";
      const fail = (e: KilnError) => {
        if (stage !== "handshake") return;
        stage = "done";
        clearTimeout(timer);
        link.closed = true;
        try { ws.close(); } catch { /* already closed */ }
        reject(e);
      };
      const timer = setTimeout(() => fail(kerr("admin_unavailable", t("api.adminWs.handshake", { seconds: Math.round(handshakeMs / 100) / 10 }))), handshakeMs);
      ws.binaryType = "arraybuffer";
      ws.addEventListener("error", () => fail(kerr("admin_unavailable", t("api.adminWs.unreachable", { reason: url.replace(/\?.*$/, "") }))));
      ws.addEventListener("close", (ev: { code: number; reason: string }) => {
        if (stage === "handshake") {
          const why = ev.reason || t("api.adminWs.closedBy", { code: ev.code });
          fail(kerr(AUTH_CLOSE.has(ev.code) ? "auth_failed" : "admin_unavailable", t("api.adminWs.closedEarly", { reason: why })));
        } else if (stage === "open") {
          stage = "done";
          link.shut(ev.reason || t("api.adminWs.closedBy", { code: ev.code }), AUTH_CLOSE.has(ev.code));
        }
      });
      ws.addEventListener("message", (ev: { data: unknown }) => {
        const f = decode(ev.data);
        link.receivedAt = Date.now();
        if (stage === "handshake") {
          if (!f) return fail(kerr("admin_unavailable", t("api.adminWs.closedEarly", { reason: "bad frame" })));
          if (f.t === "hello") {
            if (typeof f.proto === "string" && f.proto !== "kap/1") return fail(kerr("unsupported", `the server speaks ${f.proto}, this page kap/1`));
            link.hello = f;
          } else if (f.t === "ready") {
            if (!Array.isArray(f.ops) || !Array.isArray(f.topics)) return fail(kerr("admin_unavailable", t("api.adminWs.closedEarly", { reason: "bad ready" })));
            stage = "open";
            clearTimeout(timer);
            link.ready = f;
            link.limits = limitsOf(f.limits);
            link.startKeepalive();
            resolve(link);
          } else if (f.t === "error") {
            fail(kerr(errorCode(f.code), String(f.message ?? f.code)));
          }
          return;
        }
        if (stage !== "open") return;
        if (!f) {
          stage = "done";
          link.shut("bad frame from the server", false);
          try { ws.close(); } catch { /* closed */ }
          return;
        }
        if (f.t === "bye") {
          stage = "done";
          link.shut(`bye: ${String(f.reason ?? "")}`, false);
          try { ws.close(); } catch { /* closed */ }
          return;
        }
        link.frame(f);
      });
    });
  }

  private frame(f: Frame) {
    switch (f.t) {
      case "res": {
        const w = this.waiting.get(f.id as number);
        if (w) this.settle(f.id as number, w).resolve(f);
        break;
      }
      case "error": {
        const w = typeof f.id === "number" ? this.waiting.get(f.id) : undefined;
        if (w) this.settle(f.id as number, w).reject(kerr(errorCode(f.code), String(f.message ?? f.code)));
        else this.onError(`${String(f.code ?? "")}: ${String(f.message ?? "")}`);
        break;
      }
      case "push":
        this.onPush(String(f.topic), Number(f.seq), (f.data ?? null) as Json);
        break;
      case "ping":
        this.send({ t: "pong", id: f.id, ...(f.ts !== undefined ? { ts: f.ts } : {}) });
        break;
      default:
        break;  // pong, and frame types of a newer kap/1: nothing to do
    }
  }

  private settle(id: number, w: Waiter) {
    this.waiting.delete(id);
    if (w.timer) clearTimeout(w.timer);
    return w;
  }

  private send(f: Frame) {
    this.ws.send(JSON.stringify(f));
    this.sentAt = Date.now();
  }

  /** Sends a request frame (`id` is filled in) and waits for its answer. */
  request(make: (id: number) => Frame, timeoutMs: number): Promise<Frame> {
    if (this.closed) return Promise.reject(kerr("admin_unavailable", t("api.adminWs.connectionClosed")));
    const id = this.next++;
    const f = make(id);
    const json = JSON.stringify(f);
    const bytes = new TextEncoder().encode(json).length;
    if (bytes > this.limits.maxClientFrame) return Promise.reject(kerr("invalid_argument", t("api.adminWs.tooLarge", { bytes, max: this.limits.maxClientFrame })));
    return new Promise((resolve, reject) => {
      const w: Waiter = { resolve, reject, timer: null };
      w.timer = setTimeout(() => {
        if (this.waiting.get(id) === w) this.waiting.delete(id);
        reject(kerr("timeout", t("api.adminWs.timeout", { seconds: Math.round(timeoutMs / 1000) })));
      }, timeoutMs);
      this.waiting.set(id, w);
      try {
        this.ws.send(json);
        this.sentAt = Date.now();
      } catch (e) {
        this.settle(id, w).reject(kerr("admin_unavailable", e instanceof Error ? e.message : String(e)));
      }
    });
  }

  private startKeepalive() {
    const ping = Math.min(Math.max(this.limits.pingS, 1), 15) * 1000;
    const idle = Math.max(this.limits.idleS * 1000, ping * 2);
    const tick = Math.min(Math.max(ping / 4, 50), 1000);
    this.keepalive = setInterval(() => {
      if (this.closed) return;
      const now = Date.now();
      if (now - this.receivedAt > idle) {
        this.shut(t("api.adminWs.silent", { seconds: Math.round(idle / 1000) }), false);
        try { this.ws.close(); } catch { /* closed */ }
        return;
      }
      if (now - this.sentAt >= ping) {
        try {
          this.send({ t: "ping", id: this.next++, ts: now });
        } catch {
          this.shut("send failed", false);
        }
      }
    }, tick);
  }

  /** The link is done: every waiting request fails, the owner hears why. */
  shut(reason: string, final: boolean) {
    if (this.closed) return;
    this.closed = true;
    if (this.keepalive) clearInterval(this.keepalive);
    for (const [id, w] of [...this.waiting]) this.settle(id, w).reject(kerr("admin_unavailable", t("api.adminWs.connectionClosed")));
    this.onClose(reason, final);
  }

  /** Our own end: bye, then close (nothing is reported). */
  end(reason: string) {
    if (this.closed) return;
    this.closed = true;
    if (this.keepalive) clearInterval(this.keepalive);
    for (const [id, w] of [...this.waiting]) this.settle(id, w).reject(kerr("admin_unavailable", t("api.adminWs.connectionClosed")));
    try { this.send({ t: "bye", reason }); } catch { /* closed */ }
    try { this.ws.close(1000, reason); } catch { /* closed */ }
  }
}

/** `ready.limits`, each a number of at least 0, else its default (the launcher's hub reads them as unsigned integers):
 *  a missing or odd value must neither refuse every request (`maxClientFrame: null`) nor stop the keepalive (NaN). */
function limitsOf(v: unknown): KapLimits {
  const l = { ...DEFAULT_LIMITS };
  if (isObj(v)) {
    for (const k of Object.keys(l) as (keyof KapLimits)[]) {
      const x = v[k];
      if (typeof x === "number" && Number.isFinite(x) && x >= 0) l[k] = x;
    }
  }
  return l;
}

function decode(data: unknown): Frame | null {
  let text: string;
  if (typeof data === "string") text = data;
  else if (data instanceof ArrayBuffer) text = new TextDecoder().decode(data);
  else if (ArrayBuffer.isView(data)) text = new TextDecoder().decode(data);
  else return null;
  try {
    const f = JSON.parse(text) as unknown;
    return isObj(f) && typeof f.t === "string" ? (f as Frame) : null;
  } catch {
    return null;
  }
}

interface Session {
  id: string;
  serverId: ServerId;
  info: AdminSession;
  link: Link | null;
  topics: Set<string>;
  stopped: boolean;
  retry: ReturnType<typeof setTimeout> | null;
}

/** An AdminTransport over KAP/1-over-WebSocket. */
export function createWebSocketTransport(opts: WebSocketTransportOptions): WebSocketAdminTransport {
  const Ws: WebSocketClass = opts.WebSocket ?? (globalThis as unknown as { WebSocket: WebSocketClass }).WebSocket;
  const [firstMs, maxMs] = opts.backoffMs ?? [1000, 30_000];
  const handshakeMs = opts.handshakeMs ?? 10_000;
  const timeoutOf = opts.callTimeoutMs ?? ((op: string) => (opBudgetS(op) + 10) * 1000);
  const sessions = new Map<string, Session>();
  const listeners = new Set<(e: AdminHubEvent) => void>();
  let n = 0;

  const emit = (s: Session, event: AdminEvent) => {
    for (const l of [...listeners]) {
      try {
        l({ session: s.id, serverId: s.serverId, event });
      } catch (e) {
        console.error("admin ws listener", e);
      }
    }
  };

  async function open(serverId: ServerId): Promise<Link> {
    let url: string;
    try {
      url = typeof opts.url === "function" ? await opts.url(serverId) : opts.url;
    } catch (e) {
      throw isKilnError(e) ? e : kerr("admin_unavailable", e instanceof Error ? e.message : String(e));
    }
    return Link.open(Ws, url, handshakeMs);
  }

  const infoOf = (s: { id: string; serverId: ServerId }, link: Link): AdminSession => {
    const r = link.ready;
    const h = link.hello;
    const info: AdminSession = {
      session: s.id, serverId: s.serverId, transport: "ws", ops: (r.ops as string[]).map(String), topics: (r.topics as string[]).map(String),
      kiln: typeof h?.kiln === "string" ? h.kiln : null, server: isObj(h?.server) ? (h.server as AdminSession["server"]) : {},
    };
    if (isObj(r.restart)) info.restart = r.restart as unknown as KapRestart;
    return info;
  };
  const readyEvent = (info: AdminSession): AdminEvent => ({ kind: "ready", transport: "ws", ops: info.ops, topics: info.topics, ...(info.restart ? { restart: info.restart } : {}) });

  function attach(s: Session, link: Link) {
    s.link = link;
    link.onPush = (topic, seq, data) => { if (s.link === link) emit(s, { kind: "push", topic, seq, data }); };
    link.onError = (message) => { if (s.link === link) emit(s, { kind: "error", message }); };
    link.onClose = (reason, final) => {
      if (s.link !== link || s.stopped) return;
      s.link = null;
      if (final) return end(s, reason);
      emit(s, { kind: "closed", reason, retry: true });
      reconnect(s, firstMs);
    };
  }

  /** The session is over by the server's word (an auth failure): closed {retry: false}, forgotten. */
  function end(s: Session, reason: string) {
    s.stopped = true;
    if (sessions.get(s.id) === s) sessions.delete(s.id);
    emit(s, { kind: "closed", reason, retry: false });
  }

  function reconnect(s: Session, delay: number) {
    s.retry = setTimeout(async () => {
      s.retry = null;
      if (s.stopped) return;
      let link: Link;
      try {
        link = await open(s.serverId);
      } catch (e) {
        if (s.stopped) return;
        const err = isKilnError(e) ? e : kerr("admin_unavailable", String(e));
        if (err.code === "auth_failed") return end(s, err.message);
        return reconnect(s, Math.min(delay * 2, maxMs));
      }
      if (s.stopped) return link.end("disconnect");
      // pushes of the topics subscribed again come after `ready`, as from the launcher's hub
      const early: [string, number, Json][] = [];
      link.onPush = (topic, seq, data) => early.push([topic, seq, data]);
      for (const topic of s.topics) {
        try {
          const res = await link.request((id) => ({ t: "sub", id, topic }), timeoutOf("sub"));
          if (res.ok === false) emit(s, { kind: "error", message: `re-subscribing ${topic}: ${String(res.error ?? res.code)}` });
        } catch (e) {
          emit(s, { kind: "error", message: `re-subscribing ${topic}: ${isKilnError(e) ? e.message : String(e)}` });
        }
      }
      if (s.stopped) return link.end("disconnect");
      if (link.closed) return reconnect(s, Math.min(delay * 2, maxMs));
      s.info = infoOf(s, link);
      attach(s, link);
      emit(s, readyEvent(s.info));
      for (const [topic, seq, data] of early) emit(s, { kind: "push", topic, seq, data });
    }, delay);
  }

  const find = (session: string) => {
    const s = sessions.get(session);
    if (!s) throw kerr("not_found", t("api.adminWs.noSession", { session }));
    return s;
  };
  const live = (s: Session) => {
    if (!s.link || s.link.closed) throw kerr("admin_unavailable", t("api.adminWs.reconnecting"));
    return s.link;
  };

  async function sub(session: string, topic: string, on: boolean) {
    const s = find(session);
    if (on) {
      if (!s.info.topics.includes(topic)) throw kerr("invalid_argument", t("api.adminWs.noTopic", { topic }));
      s.topics.add(topic);
    } else s.topics.delete(topic);
    if (!s.link || s.link.closed) return;  // sent at the reconnect
    const res = await s.link.request((id) => ({ t: on ? "sub" : "unsub", id, topic }), timeoutOf("sub"));
    if (res.ok === false) throw kerr(errorCode(res.code), String(res.error ?? t("api.adminWs.noTopic", { topic })));
  }

  const transport: WebSocketAdminTransport = {
    name: "websocket",
    kinds: [],
    async connect(serverId) {
      for (const s of sessions.values()) if (s.serverId === serverId && !s.stopped) return s.info;
      const link = await open(serverId);
      for (const s of sessions.values()) {
        if (s.serverId === serverId && !s.stopped) {
          // another connect won the race: keep its session
          link.end("disconnect");
          return s.info;
        }
      }
      const s: Session = { id: `ws-${sanitize(serverId)}-${++n}`, serverId, info: null as unknown as AdminSession, link: null, topics: new Set(), stopped: false, retry: null };
      s.info = infoOf(s, link);
      sessions.set(s.id, s);
      attach(s, link);
      emit(s, readyEvent(s.info));
      return s.info;
    },
    async disconnect(session) {
      const s = find(session);
      s.stopped = true;
      sessions.delete(s.id);
      if (s.retry) clearTimeout(s.retry);
      s.retry = null;
      const link = s.link;
      s.link = null;
      link?.end("disconnect");
      emit(s, { kind: "closed", reason: "disconnected", retry: false });
    },
    async call<T = Json>(session: string, op: string, args: Record<string, Json>): Promise<T> {
      const link = live(find(session));
      const res = await link.request((id) => ({ t: "req", id, op, args: args ?? {} }), timeoutOf(op));
      if (res.ok === true) return (res.data ?? null) as T;
      throw kerr(errorCode(res.code), typeof res.error === "string" && res.error ? res.error : t("api.adminWs.opFailed"), { op, transport: "ws", kapCode: res.code ?? null });
    },
    subscribe: (session, topic) => sub(session, topic, true),
    unsubscribe: (session, topic) => sub(session, topic, false),
    async listen(handler) {
      listeners.add(handler);
      return () => { listeners.delete(handler); };
    },
    async disconnectAll() {
      for (const id of [...sessions.keys()]) await transport.disconnect(id).catch(() => undefined);
    },
  };
  return transport;
}
