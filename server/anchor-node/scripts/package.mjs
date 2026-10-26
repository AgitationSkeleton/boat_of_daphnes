// Builds the folder to put on the server PC: the code, the worker modules (synced from server/anchor/src), the lock file,
// production node_modules (npm ci, so exactly the locked versions), ecosystem.config.js, config.example.json, README.md.
// config.json, data/ and logs/ already in the destination are left alone (a re-deploy keeps the server's settings and content).
//   node scripts/package.mjs <destination folder>
// Needs no network beyond npm's registry for `npm ci`. ws is plain JavaScript, so the node_modules are the same on any OS.
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(dirname(fileURLToPath(import.meta.url)));
const dest = process.argv[2] && resolve(process.argv[2]);
if (!dest) { console.error('usage: node scripts/package.mjs <destination folder>'); process.exit(2); }

const run = (cmd, args, cwd) => {
  const r = spawnSync(cmd, args, { cwd, stdio: 'inherit', shell: cmd === 'npm' && process.platform === 'win32' });
  if (r.status !== 0) { console.error(`${cmd} ${args.join(' ')} failed`); process.exit(1); }
};

run(process.execPath, [join(here, 'scripts', 'sync-worker.mjs')], here);
const stage = mkdtempSync(join(tmpdir(), 'anchor-node-pack-'));
try {
  for (const name of ['src', 'worker']) cpSync(join(here, name), join(stage, name), { recursive: true });
  for (const name of ['package.json', 'package-lock.json', 'ecosystem.config.js', 'config.example.json', 'README.md']) cpSync(join(here, name), join(stage, name));
  // The tests and the sync need the repository (the worker's own sources and tests): the deployed package.json offers only start.
  const pkg = JSON.parse(readFileSync(join(stage, 'package.json'), 'utf8'));
  pkg.scripts = { start: 'node src/server.js' };
  writeFileSync(join(stage, 'package.json'), JSON.stringify(pkg, null, 2) + '\n');
  run('npm', ['ci', '--omit=dev', '--no-audit', '--no-fund'], stage);

  mkdirSync(dest, { recursive: true });
  for (const name of ['src', 'worker', 'node_modules']) rmSync(join(dest, name), { recursive: true, force: true });
  for (const name of readdirSync(stage)) cpSync(join(stage, name), join(dest, name), { recursive: true, force: true });
  mkdirSync(join(dest, 'logs'), { recursive: true });
  console.log(`packaged into ${dest}${existsSync(join(dest, 'config.json')) ? ' (kept its config.json)' : ''}`);
} finally {
  rmSync(stage, { recursive: true, force: true });
}
