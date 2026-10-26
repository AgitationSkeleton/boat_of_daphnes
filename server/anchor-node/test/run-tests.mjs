// Runs the Anchor worker's own tests (server/anchor/test) against this Node server. Each test file gets a fresh server on
// a free local port with an empty data folder (content.mjs and colors.mjs both use up the upload rate limit, as they each
// did a fresh `wrangler dev --persist-to` before). Only the processes started here are stopped, by their handles.
//   node test/run-tests.mjs [name ...]      (default: all of them)
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import net from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(dirname(fileURLToPath(import.meta.url)));
const testDir = join(here, '..', 'anchor', 'test');

// [file, how the URL is given, extra args]
const ALL = {
  room: ['room.mjs', 'ws'],
  carry: ['carry.mjs', 'ws'],
  stale_prune: ['stale_prune.mjs', 'ws'],
  token_reconnect: ['token_reconnect.mjs', 'ws', [`tok-${Date.now()}`]],
  content: ['content.mjs', 'http'],
  colors: ['colors.mjs', 'http'],
};

function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => resolve(port)); });
    s.on('error', reject);
  });
}

async function startServer() {
  const port = await freePort();
  const data = mkdtempSync(join(tmpdir(), 'anchor-node-test-'));
  const child = spawn(process.execPath, [join(here, 'src', 'server.js')], {
    cwd: here,
    env: { ...process.env, PORT: String(port), BIND: '127.0.0.1', DATA_DIR: data },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let out = '';
  child.stdout.on('data', (d) => { out += d; });
  child.stderr.on('data', (d) => { out += d; });
  for (let i = 0; i < 100; i++) {
    try { if ((await fetch(`http://127.0.0.1:${port}/health`)).ok) break; } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 100));
    if (child.exitCode !== null) throw new Error(`server exited: ${out}`);
  }
  return {
    port, child,
    stop: async () => {
      child.kill();
      await new Promise((r) => (child.exitCode !== null ? r() : child.once('exit', r)));
      rmSync(data, { recursive: true, force: true });
    },
  };
}

function run(file, args) {
  return new Promise((resolve) => {
    const p = spawn(process.execPath, [join(testDir, file), ...args], { cwd: join(testDir, '..'), stdio: 'inherit' });
    p.on('exit', (code) => resolve(code));
  });
}

const wanted = process.argv.slice(2);
const names = wanted.length ? wanted : Object.keys(ALL);
let failed = 0;
for (const name of names) {
  const entry = ALL[name];
  if (!entry) { console.log(`unknown test ${name}`); failed++; continue; }
  const [file, kind, extra = []] = entry;
  const server = await startServer();
  console.log(`\n=== ${name} (server on 127.0.0.1:${server.port}, pid ${server.child.pid})`);
  const base = `${kind === 'ws' ? 'ws' : 'http'}://127.0.0.1:${server.port}`;
  const args = kind === 'ws' && file === 'token_reconnect.mjs' ? [base, ...extra] : [base, ...extra];
  const code = await run(file, args);
  await server.stop();
  console.log(`--- ${name}: ${code === 0 ? 'PASS' : `FAIL (exit ${code})`}`);
  if (code !== 0) failed++;
}
console.log(failed ? `\n${failed} test file(s) failed` : '\nall test files passed');
process.exit(failed ? 1 : 0);
