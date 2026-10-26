// The Anchor online play server on Node: server/anchor's own Worker modules (worker/, made by scripts/sync-worker.mjs)
// behind a plain HTTP + WebSocket listener. See README.md.
import { createHash, timingSafeEqual } from 'node:crypto';
import { readFileSync } from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import { join } from 'node:path';
import { Readable } from 'node:stream';
import { pathToFileURL } from 'node:url';
import { WebSocketServer } from 'ws';
import { loadConfig, appDir } from './config.js';
import { DiskKV } from './kv.js';
import { Namespace, bindSocket, installGlobals } from './runtime.js';

const stamp = () => new Date().toISOString();
const log = (level, msg) => console.log(`${stamp()} [${level}] ${msg}`);

const MAX_BODY = 21 * 1024 * 1024;   // content uploads are limited to 20 MB by the worker; this is the read cap
const MAX_SMALL_BODY = 1024 * 1024;  // anything else
const MAX_WS_MESSAGE = 1024 * 1024;  // Cloudflare's WebSocket message limit; the room itself refuses over 64 KB

const config = loadConfig();
installGlobals();
const { default: worker, Room, Lobby } = await import(pathToFileURL(join(appDir, 'worker', 'index.js')).href);

const kv = new DiskKV(join(config.dataDir, 'content'), { maxBytes: config.maxContentBytes, log });
const env = {
  ...Object.fromEntries(Object.entries(config.env).map(([k, v]) => [k, String(v)])),
  PROTOCOLS: config.protocols,
  IP_SALT: config.ipSalt,
  CONTENT: kv,
  // The worker's optional hint to another server (index.js); a fallback server leaves these unset.
  ...(config.fallbackHost ? { FALLBACK_HOST: config.fallbackHost, FALLBACK_MODE: config.fallbackMode || '' } : {}),
};
const logger = (level, msg) => log(level, msg);
env.ROOM = new Namespace('Room', Room, env, config.salt, logger);
env.LOBBY = new Namespace('Lobby', Lobby, env, config.salt, logger);

// ---- Node request <-> Fetch API -------------------------------------------------------------------------------------------
function headersFrom(req) {
  const h = new Headers();
  for (let i = 0; i < req.rawHeaders.length; i += 2) {
    const name = req.rawHeaders[i].toLowerCase();
    if (name === 'cf-connecting-ip' || name === 'x-forwarded-for') continue; // from the socket only: a client cannot pick its address
    try { h.append(name, req.rawHeaders[i + 1]); } catch { /* an invalid header is dropped */ }
  }
  h.set('cf-connecting-ip', clientIp(req));
  return h;
}

function clientIp(req) {
  return String(req.socket.remoteAddress || 'local').replace(/^::ffff:/, '');
}

async function readBody(req, limit) {
  const parts = [];
  let size = 0;
  for await (const chunk of req) {
    size += chunk.length;
    if (size > limit) throw Object.assign(new Error('too large'), { status: 413 });
    parts.push(chunk);
  }
  return Buffer.concat(parts);
}

async function toRequest(req, withBody) {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  const init = { method: req.method, headers: headersFrom(req) };
  if (withBody && req.method !== 'GET' && req.method !== 'HEAD') {
    const body = await readBody(req, url.pathname.startsWith('/content/') ? MAX_BODY : MAX_SMALL_BODY);
    if (body.length) init.body = body;
  }
  const request = new Request(url, init);
  // What Cloudflare's request.cf carries; the room log shows it as "From".
  Object.defineProperty(request, 'cf', { value: { colo: 'fallback', country: '' } });
  return request;
}

function plain(res, status, text) {
  res.writeHead(status, { 'content-type': 'text/plain', 'access-control-allow-origin': '*' });
  res.end(text);
}

// ---- Admin routes ---------------------------------------------------------------------------------------------------------
// /_status, /_admin/purge-content and /_admin/sweep: from this machine, or with the header x-admin-key equal to config.adminKey
// (no key configured: this machine only). Anything else, and every failure, is a plain 404; failed attempts from one address
// are limited so the key cannot be guessed (once over the limit even the right key gets 404 until the minute is over).
const FAILED_LIMIT = 10;
const FAILED_WINDOW_MS = 60 * 1000;
const failedAttempts = new Map(); // address -> { count, since }
const digest = (s) => createHash('sha256').update(String(s)).digest();
const isLoopback = (ip) => ip === '127.0.0.1' || ip === '::1';

function lockedOut(ip) {
  const f = failedAttempts.get(ip);
  if (!f) return false;
  if (Date.now() - f.since > FAILED_WINDOW_MS) { failedAttempts.delete(ip); return false; }
  return f.count >= FAILED_LIMIT;
}
function noteFailure(ip) {
  const now = Date.now();
  const f = failedAttempts.get(ip);
  if (f && now - f.since <= FAILED_WINDOW_MS) f.count++;
  else {
    if (failedAttempts.size > 10000) for (const [k, v] of failedAttempts) if (now - v.since > FAILED_WINDOW_MS) failedAttempts.delete(k);
    if (failedAttempts.size <= 10000) failedAttempts.set(ip, { count: 1, since: now });
  }
}
function authorized(req) {
  const ip = clientIp(req);
  if (isLoopback(ip)) return true;
  if (!config.adminKey) return false;          // admin routes are off for other addresses
  if (lockedOut(ip)) return false;
  const given = req.headers['x-admin-key'];
  const ok = typeof given === 'string' && timingSafeEqual(digest(given), digest(config.adminKey));
  if (!ok) noteFailure(ip);
  return ok;
}

function json(res, status, value) {
  res.writeHead(status, { 'content-type': 'application/json' });
  res.end(JSON.stringify(value));
}

function admin(req, res, url) {
  if (!authorized(req)) return plain(res, 404, 'not found');
  const route = `${req.method} ${url.pathname}`;
  if (route === 'GET /_status') {
    return json(res, 200, {
      uptime: Math.round(process.uptime()), rooms: env.ROOM.stats(), lobby: env.LOBBY.stats(), contentBytes: kv.bytes(),
      storage: { contentBytes: kv.bytes(), contentFiles: kv.index.size, maxContentBytes: kv.maxBytes, ...kv.stats },
      connections: wss.clients.size, rss: process.memoryUsage().rss,
    });
  }
  if (route === 'POST /_admin/purge-content') {
    const freed = kv.purge();
    log('info', `admin: purged all stored content, ${freed} bytes freed`);
    return json(res, 200, { ok: true, bytesFreed: freed });
  }
  if (route === 'POST /_admin/sweep') {
    const removed = kv.sweep();
    log('info', `admin: expiry sweep removed ${removed} entries`);
    return json(res, 200, { ok: true, removed });
  }
  if (url.pathname === '/_status' || url.pathname === '/_admin/purge-content' || url.pathname === '/_admin/sweep') return plain(res, 405, 'method not allowed');
  return plain(res, 404, 'not found');
}

async function handleRequest(req, res) {
  try {
    const url = new URL(req.url, 'http://localhost');
    if (url.pathname === '/_status' || url.pathname.startsWith('/_admin/')) return admin(req, res, url);
    const request = await toRequest(req, true);
    const response = await worker.fetch(request, env);
    const headers = {};
    response.headers.forEach((v, k) => { headers[k] = v; });
    res.writeHead(response.status, headers);
    if (!response.body || req.method === 'HEAD') return res.end();
    Readable.fromWeb(response.body).on('error', () => res.destroy()).pipe(res);
  } catch (e) {
    if (res.headersSent) return res.destroy();
    if (e && e.status === 413) return plain(res, 413, 'too large');
    log('error', `request ${req.method} ${String(req.url).slice(0, 80)}: ${(e && e.stack) || e}`);
    plain(res, 500, 'server error');
  }
}

// Plain HTTP/ws by default; wss/https when config.json names a certificate and key (tlsCert, tlsKey: PEM files).
const httpServer = config.tlsCert && config.tlsKey
  ? https.createServer({ cert: readFileSync(config.tlsCert), key: readFileSync(config.tlsKey), minVersion: 'TLSv1.2' }, handleRequest)
  : http.createServer(handleRequest);
httpServer.requestTimeout = 5 * 60 * 1000; // a 20 MB upload on a slow link
httpServer.headersTimeout = 20 * 1000;
httpServer.keepAliveTimeout = 15 * 1000;

// ---- WebSockets --------------------------------------------------------------------------------------------------------------
const wss = new WebSocketServer({ noServer: true, maxPayload: MAX_WS_MESSAGE, perMessageDeflate: false });
const perIp = new Map();

function refuse(socket, status, message) {
  const body = JSON.stringify({ error: message });
  socket.end(`HTTP/1.1 ${status} ${http.STATUS_CODES[status] || 'Error'}\r\ncontent-type: application/json\r\naccess-control-allow-origin: *\r\ncontent-length: ${Buffer.byteLength(body)}\r\nconnection: close\r\n\r\n${body}`);
}

httpServer.on('upgrade', async (req, socket, head) => {
  socket.on('error', () => {});
  try {
    const ip = clientIp(req);
    if (wss.clients.size >= config.maxConnections) return refuse(socket, 503, 'the server is full');
    if ((perIp.get(ip) || 0) >= config.maxConnectionsPerIp) return refuse(socket, 429, 'too many connections from this address');
    const request = await toRequest(req, false);
    const response = await worker.fetch(request, env);
    const pair = response.webSocket;
    if (response.status !== 101 || !pair || !pair.server) {
      // The worker answered with an HTTP response (a bad room name, a missing upgrade header ...): pass it on.
      const text = await response.text();
      const lines = [`HTTP/1.1 ${response.status} ${http.STATUS_CODES[response.status] || 'Error'}`];
      response.headers.forEach((v, k) => { if (k !== 'content-length') lines.push(`${k}: ${v}`); });
      lines.push(`content-length: ${Buffer.byteLength(text)}`, 'connection: close');
      return socket.end(`${lines.join('\r\n')}\r\n\r\n${text}`);
    }
    wss.handleUpgrade(req, socket, head, (ws) => {
      wss.emit('connection', ws, req);
      perIp.set(ip, (perIp.get(ip) || 0) + 1);
      ws.isAlive = true;
      ws.on('pong', () => { ws.isAlive = true; });
      ws.on('close', () => {
        const n = (perIp.get(ip) || 1) - 1;
        if (n <= 0) perIp.delete(ip); else perIp.set(ip, n);
      });
      bindSocket(pair.server, ws);
    });
  } catch (e) {
    log('error', `upgrade: ${(e && e.stack) || e}`);
    try { refuse(socket, 500, 'server'); } catch { /* gone */ }
  }
});

// Protocol-level heartbeat: a connection that does not answer two pings in a row is dropped (a player whose link died
// would otherwise keep a seat until the TCP timeout).
setInterval(() => {
  for (const ws of wss.clients) {
    if (!ws.isAlive) { ws.terminate(); continue; }
    ws.isAlive = false;
    try { ws.ping(); } catch { /* closing */ }
  }
}, 30 * 1000).unref();

// Housekeeping: forget idle rooms, remove expired content.
setInterval(() => {
  env.ROOM.sweep(config.maxRoomsIdleMinutes * 60 * 1000);
  env.LOBBY.sweep(Infinity);
}, 5 * 60 * 1000).unref();
setInterval(() => {
  const removed = kv.sweep();
  if (removed) log('info', `removed ${removed} expired content entries`);
}, 60 * 60 * 1000).unref();
setInterval(() => {
  const s = env.ROOM.stats();
  log('info', `rooms ${s.objects}, players connected ${wss.clients.size}, rss ${Math.round(process.memoryUsage().rss / 1048576)} MB`);
}, 10 * 60 * 1000).unref();

// ---- Start and stop ------------------------------------------------------------------------------------------------------------
httpServer.on('error', (e) => {
  log('fatal', `cannot listen on ${config.bind}:${config.port}: ${e.message}`);
  process.exit(1);
});
httpServer.listen(config.port, config.bind, () => {
  log('info', `Anchor fallback server listening on ${config.bind}:${config.port} (public name ${config.publicHost}, ${config.tlsCert ? 'TLS (wss/https, TLS 1.2 or newer)' : 'NO TLS (plain ws/http)'}); data ${config.dataDir}; content cap ${config.maxContentBytes} bytes (${kv.bytes()} stored); admin routes ${config.adminKey ? 'on (key set)' : 'loopback only'}; node ${process.version}`);
});

function shutdown(signal) {
  log('info', `${signal}: closing`);
  httpServer.close();
  for (const ws of wss.clients) { try { ws.close(1001, 'server restarting'); } catch { /* gone */ } }
  setTimeout(() => process.exit(0), 1500).unref();
}
process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('unhandledRejection', (e) => log('error', `unhandled rejection: ${(e && e.stack) || e}`));
process.on('uncaughtException', (e) => { log('fatal', `uncaught: ${(e && e.stack) || e}`); process.exit(1); });
