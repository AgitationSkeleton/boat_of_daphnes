// What the Node server adds to the worker, checked on a fresh local server: the room list, PING auto-response, an oversize message, the redirect hint is absent
// by default, and the per-address connection cap. Usage: node test/node_extra.mjs
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import net from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(dirname(fileURLToPath(import.meta.url)));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failures++; };
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => res(port)); }); });

const port = await freePort();
const data = mkdtempSync(join(tmpdir(), 'anchor-node-extra-'));
const cfg = join(data, 'config.json');
(await import('node:fs')).writeFileSync(cfg, JSON.stringify({ maxConnectionsPerIp: 6, env: { EXTRA_FOR_THE_WORKER: 'x' } }));
const child = spawn(process.execPath, [join(here, 'src', 'server.js')], { cwd: here, env: { ...process.env, CONFIG: cfg, PORT: String(port), BIND: '127.0.0.1', DATA_DIR: data }, stdio: 'ignore' });
for (let i = 0; i < 100; i++) { try { if ((await fetch(`http://127.0.0.1:${port}/health`)).ok) break; } catch {} await wait(100); }
const base = `127.0.0.1:${port}`;
const room = `extra-${Date.now()}-SECRETNAME`;

function join_(name, extra = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://${base}/ws?room=${room}`);
    const p = { ws, inbox: [] };
    ws.onmessage = (e) => p.inbox.push(JSON.parse(e.data));
    ws.onerror = reject;
    ws.onopen = () => { ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1, clientState: { name, teamId: 'default', isSaveLoaded: true, ...extra }, roomState: { listed: 1, title: 'Extra room' } })); resolve(p); };
  });
}

try {
  const health = await (await fetch(`http://${base}/health`)).json();
  check(health.ok === true && !('fallback' in health), '/health as the worker answers it, no fallback hint by default');
  const a = await join_('Ann');
  await wait(300);
  check(a.inbox.some((m) => m.type === 'ALL_CLIENT_STATE'), 'handshake answered');
  a.ws.send('{"type":"PING"}');
  await wait(200);
  check(a.inbox.some((m) => m.type === 'PONG'), 'PING gets PONG without waking the room');
  const rooms = await (await fetch(`http://${base}/rooms`)).json();
  check(rooms.rooms.length === 1 && rooms.rooms[0].title === 'Extra room' && rooms.rooms[0].players === 1, 'a listed room is in /rooms');
  a.ws.send('x'.repeat(70 * 1024));
  await wait(300);
  check(a.inbox.some((m) => m.type === 'ERROR' && /too large/.test(m.message)), 'a message over 64 KB is refused by the room');
  const b = await join_('Bob');
  await wait(300);
  a.ws.send(JSON.stringify({ type: 'CHAT', text: 'hi' }));
  await wait(200);
  check(b.inbox.some((m) => m.type === 'CHAT' && m.clientId === 1), 'a message is relayed to the room');
  b.ws.close();
  a.ws.close();
  await wait(800);
  check((await fetch(`http://${base}/rooms`).then((r) => r.json())).rooms.length === 0, 'the room list empties when the room does');
  // the per-address connection cap (6 here)
  const many = [];
  let refused = 0;
  for (let i = 0; i < 9; i++) {
    await new Promise((res) => { const ws = new WebSocket(`ws://${base}/ws?room=cap-${i}`); ws.onopen = () => { many.push(ws); res(); }; ws.onerror = () => { refused++; res(); }; });
  }
  check(many.length === 6 && refused === 3, `at most 6 connections per address (${many.length} open, ${refused} refused)`);
  many.forEach((w) => w.close());
  const bad = await fetch(`http://${base}/ws?room=`, { headers: { Upgrade: 'websocket' } }).catch(() => null);
  const st = await (await fetch(`http://${base}/_status`)).json();
  check(typeof st.uptime === 'number', '/_status from this machine');
} finally {
  child.kill();
  await wait(300);
  rmSync(data, { recursive: true, force: true });
}
console.log(failures ? `${failures} failed` : 'all passed');
process.exit(failures ? 1 : 0);
