// Copies the Cloudflare Worker's own modules (server/anchor/src) into ./worker, ready for Node: the one import that only
// exists on Cloudflare ('cloudflare:workers') is pointed at src/cf_workers.js, and room_log.js is overlay/room_log.js (no
// log, as the public worker's), or the operator's own named by ROOM_LOG_JS. Nothing else is changed, so a change to the worker's room.js, lobby.js, content.js or index.js
// carries over by running this again (npm run sync; npm start does it first).
//   node scripts/sync-worker.mjs [path to server/anchor/src]
import { copyFileSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(dirname(fileURLToPath(import.meta.url)));
const from = resolve(process.argv[2] || join(here, '..', 'anchor', 'src'));
const to = join(here, 'worker');
rmSync(to, { recursive: true, force: true });
mkdirSync(to, { recursive: true });
let n = 0;
for (const name of readdirSync(from)) {
  if (!name.endsWith('.js')) continue;
  if (name === 'room_log.js') continue; // replaced below
  let text = readFileSync(join(from, name), 'utf8');
  const swapped = text.replace(/from 'cloudflare:workers'/g, "from '../src/cf_workers.js'");
  if (swapped !== text && /cloudflare:workers/.test(swapped)) throw new Error(`${name}: a cloudflare:workers import was left`);
  writeFileSync(join(to, name), swapped);
  n++;
}
const roomLog = process.env.ROOM_LOG_JS ? resolve(process.env.ROOM_LOG_JS) : join(here, 'overlay', 'room_log.js');
copyFileSync(roomLog, join(to, 'room_log.js'));
writeFileSync(join(to, 'package.json'), '{ "type": "module" }\n'); // the app root is CommonJS (pm2 reads ecosystem.config.js there)
console.log(`synced ${n} worker modules from ${from} into ${to} (room_log.js from ${roomLog})`);
