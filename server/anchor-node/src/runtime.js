// A small stand-in for the Cloudflare Workers runtime, enough for server/anchor's own modules (room.js, lobby.js,
// content.js, index.js) to run unchanged on Node: Durable Object classes (one live object per name, in memory), the
// WebSocket hibernation API (acceptWebSocket / getWebSockets / serializeAttachment / auto-response), and a Response
// that may carry a WebSocket (status 101).
//
// Durable Object semantics kept: one object per (class, name); its events (fetch, message, close, error) run one at a
// time, in arrival order (the real runtime's input gates do the same around storage calls). Not kept: storage that
// survives a restart (rooms are in memory; the sockets they describe die with the process anyway) and hibernation.

import { createHmac } from 'node:crypto';

// ---- Response with a WebSocket ---------------------------------------------------------------------------------------
const NativeResponse = globalThis.Response;
export class WorkerResponse extends NativeResponse {
  constructor(body, init) {
    if (init && init.status === 101) {
      const { webSocket, status, ...rest } = init;
      super(null, { ...rest, status: 200 });
      Object.defineProperty(this, 'status', { value: 101 });
      Object.defineProperty(this, 'webSocket', { value: webSocket });
    } else {
      super(body, init);
    }
  }
}

// ---- WebSocketPair -----------------------------------------------------------------------------------------------------
// The "server" end is what the Durable Object code holds; the real network socket (from `ws`) is bound to it later by
// the HTTP layer (Namespace.bind).
export class ServerSocket {
  constructor() {
    this.attachment = null;
    this.real = null;
    this.closed = false;
  }
  serializeAttachment(v) { this.attachment = v === undefined ? null : structuredClone(v); }
  deserializeAttachment() { return this.attachment === null ? null : structuredClone(this.attachment); }
  send(data) {
    if (this.closed || !this.real || this.real.readyState !== 1) throw new Error('socket is not open');
    this.real.send(data);
  }
  close(code, reason) {
    if (this.closed) return;
    this.closed = true;
    if (this.real) {
      try { this.real.close(code || 1000, reason || ''); } catch { try { this.real.terminate(); } catch { /* gone */ } }
    }
  }
}
export class WebSocketPair {
  constructor() {
    this[1] = new ServerSocket();
    this[0] = { clientEnd: true, server: this[1] };
  }
}
export class WebSocketRequestResponsePair {
  constructor(request, response) { this.request = request; this.response = response; }
}

// ---- Durable Objects ---------------------------------------------------------------------------------------------------
export class DurableObject {
  constructor(ctx, env) { this.ctx = ctx; this.env = env; }
}

class ObjectId {
  constructor(name, hex) { this.name = name; this.hex = hex; }
  toString() { return this.hex; }
}

// One object's context: the state API a Durable Object sees as `this.ctx`.
class ObjectContext {
  constructor(id) {
    this.id = id;
    this.sockets = new Set();
    this.autoResponse = null;
    this.mem = new Map();
    this.pending = new Set();
    this.lastActive = Date.now();
    this.storage = {
      get: async (k) => (this.mem.has(k) ? structuredClone(this.mem.get(k)) : undefined),
      put: async (k, v) => { this.mem.set(k, structuredClone(v)); },
      delete: async (k) => this.mem.delete(k),
    };
  }
  setWebSocketAutoResponse(pair) { this.autoResponse = pair; }
  acceptWebSocket(ws) { ws.entry = this.entry; this.sockets.add(ws); }
  getWebSockets() { return [...this.sockets].filter((w) => !w.closed); }
  waitUntil(p) {
    const q = Promise.resolve(p).catch(() => {}).finally(() => this.pending.delete(q));
    this.pending.add(q);
  }
}

export class Namespace {
  constructor(name, cls, env, salt, log) {
    this.name = name; this.cls = cls; this.env = env; this.salt = salt; this.log = log;
    this.objects = new Map(); // hex id -> { obj, ctx, run }
  }
  idFromName(name) {
    return new ObjectId(name, createHmac('sha256', this.salt).update(`${this.name}\0${name}`).digest('hex'));
  }
  get(id) {
    const ns = this;
    return {
      fetch(input, init) {
        const request = input instanceof Request ? input : new Request(input, init);
        const e = ns.instance(id);
        return e.run(() => e.obj.fetch(request));
      },
    };
  }
  instance(id) {
    let e = this.objects.get(id.hex);
    if (e) { e.ctx.lastActive = Date.now(); return e; }
    const ctx = new ObjectContext(id);
    const obj = new this.cls(ctx, this.env);
    e = {
      obj, ctx, tail: Promise.resolve(),
      // Events of one object run one at a time, in arrival order. A failing event is logged, not fatal.
      run: (fn) => {
        const result = e.tail.then(fn);
        e.tail = result.catch((err) => this.log('error', `${this.name} event failed: ${(err && err.stack) || err}`));
        return result;
      },
    };
    ctx.entry = e;
    this.objects.set(id.hex, e);
    return e;
  }
  // Drop objects with no sockets that have been idle a while (a room's offline seats are pruned after 30 minutes anyway).
  sweep(idleMs) {
    const now = Date.now();
    for (const [hex, e] of this.objects) {
      if (e.ctx.getWebSockets().length === 0 && now - e.ctx.lastActive > idleMs) this.objects.delete(hex);
    }
  }
  stats() {
    let sockets = 0;
    for (const e of this.objects.values()) sockets += e.ctx.getWebSockets().length;
    return { objects: this.objects.size, sockets };
  }
}

// The HTTP layer calls this once the real socket is upgraded: wires its events to the object that accepted `server`.
export function bindSocket(server, real) {
  const e = server.entry;
  server.real = real;
  const auto = e.ctx.autoResponse;
  real.on('message', (data, isBinary) => {
    if (server.closed) return;
    const payload = isBinary ? data : data.toString('utf8');
    if (auto && typeof payload === 'string' && payload === auto.request) {
      try { real.send(auto.response); } catch { /* closing */ }
      return;
    }
    e.ctx.lastActive = Date.now();
    e.run(() => e.obj.webSocketMessage(server, payload)).catch(() => {});
  });
  let ended = false;
  const end = (kind) => {
    if (ended) return;
    ended = true;
    server.closed = true;
    e.run(async () => {
      try { await (kind === 'error' ? e.obj.webSocketError(server) : e.obj.webSocketClose(server)); }
      finally { e.ctx.sockets.delete(server); e.ctx.lastActive = Date.now(); }
    }).catch(() => {});
  };
  real.on('close', () => end('close'));
  real.on('error', () => end('error'));
}

export function installGlobals() {
  globalThis.Response = WorkerResponse;
  globalThis.WebSocketPair = WebSocketPair;
  globalThis.WebSocketRequestResponsePair = WebSocketRequestResponsePair;
}
