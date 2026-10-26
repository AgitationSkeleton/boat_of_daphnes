// The fallback server's hardening, on fresh local servers: the content storage cap (least recently used goes first), the
// admin routes (key, loopback, rate limit) and TLS with a throwaway self-signed certificate that a client trusts by pin.
// Usage: node test/hardening.mjs      (needs `openssl` on the PATH for the throwaway certificate)
import { spawn, spawnSync } from 'node:child_process';
import { X509Certificate, createHash } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import https from 'node:https';
import net from 'node:net';
import { networkInterfaces, tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import tls from 'node:tls';
import { fileURLToPath } from 'node:url';
import { WebSocket as WS } from 'ws';
import { DiskKV } from '../src/kv.js';

const here = dirname(dirname(fileURLToPath(import.meta.url)));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failures++; };
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => res(port)); }); });
const temp = mkdtempSync(join(tmpdir(), 'anchor-node-hard-'));
const children = [];

async function startServer(name, config, { scheme = 'http', ca } = {}) {
  const dir = join(temp, name);
  mkdirSync(dir, { recursive: true });
  const port = await freePort();
  const cfg = join(dir, 'config.json');
  writeFileSync(cfg, JSON.stringify({ ...config }));
  const child = spawn(process.execPath, [join(here, 'src', 'server.js')], {
    cwd: here, env: { ...process.env, CONFIG: cfg, PORT: String(port), BIND: '0.0.0.0' }, stdio: ['ignore', 'pipe', 'pipe'],
  });
  children.push(child);
  const server = { dir, port, child, out: '' };
  child.stdout.on('data', (d) => { server.out += d; });
  child.stderr.on('data', (d) => { server.out += d; });
  for (let i = 0; i < 100 && child.exitCode === null; i++) {
    try {
      if (scheme === 'http' ? (await fetch(`http://127.0.0.1:${port}/health`)).ok : await httpsGet(port, '/health', ca)) break;
    } catch { /* not up yet */ }
    await wait(100);
  }
  return server;
}

function httpsGet(port, path, ca) {
  return new Promise((resolve, reject) => {
    const req = https.get({ host: 'localhost', port, path, ca }, (res) => { let b = ''; res.on('data', (d) => (b += d)); res.on('end', () => resolve(res.statusCode === 200 ? b : false)); });
    req.on('error', reject);
  });
}

// ---- 1. the storage cap ------------------------------------------------------------------------------------------------------
{
  const dir = join(temp, 'kv');
  const logs = [];
  const kv = new DiskKV(dir, { maxBytes: 1000, log: (l, m) => logs.push(m) });
  const blob = (n, ch) => Buffer.alloc(n, ch);
  await kv.put('c:a', blob(300, 1), { expirationTtl: 3600 }); await wait(15);
  await kv.put('c:b', blob(300, 2), { expirationTtl: 3600 }); await wait(15);
  await kv.put('c:c', blob(300, 3), { expirationTtl: 3600 }); await wait(15);
  check(kv.bytes() === 900 && kv.stats.evictedFiles === 0, 'under the cap nothing is evicted');
  await kv.get('c:a', 'arrayBuffer'); await wait(15);          // a is now the most recently used
  await kv.put('c:d', blob(300, 4), { expirationTtl: 3600 });  // 1200 > 1000: down to 900
  check((await kv.get('c:b', 'arrayBuffer')) === null, 'the least recently used (b, not the just-read a) was evicted');
  check((await kv.get('c:a', 'arrayBuffer')) !== null && (await kv.get('c:c', 'arrayBuffer')) !== null && (await kv.get('c:d', 'arrayBuffer')) !== null, 'the others are kept, the new one included');
  check(kv.bytes() === 900 && kv.stats.evictedFiles === 1 && kv.stats.evictedBytes === 300, `the total is back under 90% (${kv.bytes()} bytes), one eviction counted`);
  check(logs.length === 1 && /evicted 1 /.test(logs[0]) && !/c_/.test(logs[0]), 'one summary line logged, no file names');
  // a rate counter is not content: neither counted nor evicted
  await kv.put('rl:abc:1', Buffer.from('{"n":1,"b":1}'), { expirationTtl: 7200 });
  check(kv.bytes() === 900, 'rate counters do not count against the cap');
  await kv.put('c:e', blob(950, 5), { expirationTtl: 3600 });  // bigger than 90%: everything else goes, itself stays
  check((await kv.get('c:e', 'arrayBuffer')) !== null && kv.index.size === 1, 'the item being written is never evicted');
  check((await kv.get('rl:abc:1', 'string')) !== null, 'the rate counter survived the eviction');
  // the order survives a restart (mtime), and the total is scanned
  const dir2 = join(temp, 'kv3');
  const first = new DiskKV(dir2, { maxBytes: 1000 });
  await first.put('c:h', blob(400, 1), { expirationTtl: 3600 }); await wait(15);
  await first.put('c:i', blob(400, 2), { expirationTtl: 3600 }); await wait(15);
  await first.get('c:h', 'arrayBuffer'); await wait(15);
  const again = new DiskKV(dir2, { maxBytes: 1000 });
  check(again.bytes() === 800 && again.index.size === 2, 'the total is scanned at start');
  await again.put('c:j', blob(300, 3), { expirationTtl: 3600 });
  check((await again.get('c:i', 'arrayBuffer')) === null && (await again.get('c:h', 'arrayBuffer')) !== null, 'after a restart the last-use order still decides (i was older than the read h)');
  // purge
  const freed = again.purge();
  check(freed > 0 && again.bytes() === 0 && again.index.size === 0, `purge frees everything (${freed} bytes)`);
  // expiry
  const short = new DiskKV(join(temp, 'kv2'), { maxBytes: 1000 });
  await short.put('c:x', blob(100, 1), { expirationTtl: 3600 });
  writeFileSync(short.file('c:x') + '.exp', String(Date.now() - 1000));
  check(short.sweep() === 1 && short.bytes() === 0, 'the expiry sweep still removes expired content and the total follows');
}

// ---- 2. the admin routes ----------------------------------------------------------------------------------------------------
{
  const KEY = 'test-admin-key-0123456789abcdef';
  const s = await startServer('admin', { adminKey: KEY, maxContentBytes: 5000 });
  const lan = Object.values(networkInterfaces()).flat().find((i) => i && i.family === 'IPv4' && !i.internal);
  const base = `http://127.0.0.1:${s.port}`;
  const st = await (await fetch(`${base}/_status`)).json();
  check(st.storage && st.storage.maxContentBytes === 5000 && 'evictedFiles' in st.storage && 'evictions' in st.storage && typeof st.uptime === 'number', '/_status from loopback needs no key and shows the cap and eviction counts');
  const hash = createHash('sha256').update('hello').digest('hex');
  const put = await fetch(`${base}/content/${hash}?kind=metadata`, { method: 'PUT', body: 'hello' });
  check([201, 400, 415].includes(put.status), `content PUT answers (${put.status})`);
  const sw = await fetch(`${base}/_admin/sweep`, { method: 'POST' });
  check(sw.status === 200 && (await sw.json()).removed === 0, 'POST /_admin/sweep from loopback, no key');
  const pg = await fetch(`${base}/_admin/purge-content`, { method: 'POST' });
  const pgj = await pg.json();
  check(pg.status === 200 && typeof pgj.bytesFreed === 'number', 'POST /_admin/purge-content from loopback, no key');
  check((await fetch(`${base}/_admin/nothing`, { method: 'POST' })).status === 404, 'any other /_admin/ path is 404');
  check((await fetch(`${base}/_admin/sweep`)).status === 405, 'a wrong method on a real route is refused (from an authorized caller)');
  if (!lan) {
    console.log('skip  no non-loopback IPv4 address on this machine: the remote-address checks are not run');
  } else {
    const remote = `http://${lan.address}:${s.port}`;
    const call = (path, method = 'GET', key) => fetch(`${remote}${path}`, { method, headers: key ? { 'x-admin-key': key } : {} });
    check((await call('/_status')).status === 404, 'remote /_status without a key: 404');
    check((await call('/_status', 'GET', 'wrong-key')).status === 404, 'remote /_status with a wrong key: 404');
    const ok = await call('/_status', 'GET', KEY);
    check(ok.status === 200 && 'storage' in (await ok.json()), 'remote /_status with the right key: 200');
    const pk = await call('/_admin/purge-content', 'POST', KEY);
    check(pk.status === 200 && 'bytesFreed' in (await pk.json()), 'remote purge with the right key: 200');
    check((await call('/_admin/sweep', 'POST', KEY)).status === 200, 'remote sweep with the right key: 200');
    check((await call('/_admin/other', 'POST', KEY)).status === 404, 'remote unknown admin route with the key: 404');
    let last = 0;
    for (let i = 0; i < 14; i++) last = (await call('/_status', 'GET', `guess-${i}`)).status;
    check(last === 404, 'guesses all answer 404');
    check((await call('/_status', 'GET', KEY)).status === 404, 'after 10 failures a minute even the right key is locked out (404)');
    check((await fetch(`${base}/_status`)).status === 200, 'loopback is not affected by the lockout');
    check((await fetch(`${remote}/health`)).status === 200, 'normal routes are not affected by the lockout');
  }
  const n = await startServer('nokey', {});
  check((await fetch(`http://127.0.0.1:${n.port}/_status`)).status === 200, 'no adminKey: loopback /_status still works');
  if (lan) {
    const r = await fetch(`http://${lan.address}:${n.port}/_status`, { headers: { 'x-admin-key': 'anything' } });
    check(r.status === 404, 'no adminKey: remote /_status is 404');
  }
  check(/admin routes loopback only/.test(n.out) && /NO TLS/.test(n.out), 'the start line says no TLS, loopback only admin');
}

// ---- 3. TLS with a pinned throwaway certificate -------------------------------------------------------------------------------
{
  const tlsDir = join(temp, 'tlsserver', 'tls');
  mkdirSync(tlsDir, { recursive: true });
  const gen = spawnSync('openssl', ['req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', join(tlsDir, 'key.pem'),
    '-out', join(tlsDir, 'cert.pem'), '-days', '2', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], { encoding: 'utf8' });
  if (gen.status !== 0 || !existsSync(join(tlsDir, 'cert.pem'))) {
    check(false, `openssl could not make the throwaway certificate: ${gen.stderr || gen.error}`);
  } else {
    const pem = readFileSync(join(tlsDir, 'cert.pem'), 'utf8');
    const spki = (der) => createHash('sha256').update(new X509Certificate(der).publicKey.export({ type: 'spki', format: 'der' })).digest('hex');
    const pin = spki(pem);
    // relative paths in config.json resolve against its folder (the server is started with another working directory)
    const s = await startServer('tlsserver', { tlsCert: './tls/cert.pem', tlsKey: './tls/key.pem' }, { scheme: 'https', ca: pem });
    check(/TLS \(wss\/https, TLS 1\.2 or newer\)/.test(s.out), 'the start line says TLS');
    check((await httpsGet(s.port, '/health', pem)) !== false, 'https /health with the certificate trusted');
    const refusedPlain = await fetch(`http://127.0.0.1:${s.port}/health`).then(() => false, () => true);
    check(refusedPlain, 'plain http to the TLS port is not answered');
    const connect = (pinToUse) => new Promise((resolve) => {
      const ws = new WS(`wss://localhost:${s.port}/ws?room=pin-test`, {
        ca: pem, // so Node's own chain check passes; the pin below is what the game's client relies on alone
        checkServerIdentity: (host, cert) => (spki(cert.raw) === pinToUse ? undefined : new Error('certificate pin mismatch')),
      });
      ws.on('open', () => { ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1, clientState: { name: 'Pin', teamId: 'default', isSaveLoaded: true }, roomState: {} })); });
      ws.on('message', () => { ws.close(); resolve('connected'); });
      ws.on('error', (e) => resolve(`error: ${e.message}`));
      setTimeout(() => resolve('timeout'), 5000);
    });
    check((await connect(pin)) === 'connected', 'wss with the right pin joins a room');
    const wrong = await connect('0'.repeat(64));
    check(/pin mismatch/.test(wrong), `wss with a wrong pin is refused (${wrong})`);
    const old = await new Promise((resolve) => {
      const sock = tls.connect({ host: '127.0.0.1', port: s.port, rejectUnauthorized: false, maxVersion: 'TLSv1.1', minVersion: 'TLSv1' }, () => resolve('connected'));
      sock.on('error', () => resolve('refused'));
    });
    check(old === 'refused', 'TLS 1.1 is refused (minimum TLS 1.2)');
    console.log(`info  throwaway SPKI pin was ${pin}`);
  }
}

for (const c of children) { if (c.exitCode === null) c.kill(); }
await wait(300);
rmSync(temp, { recursive: true, force: true });
console.log(failures ? `${failures} FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
