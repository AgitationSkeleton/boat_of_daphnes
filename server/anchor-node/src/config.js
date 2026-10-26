// The server's settings: config.json (next to package.json, or the file named by CONFIG), with environment variables over
// it. Nothing secret is in the code or in config.example.json; whatever an operator keeps secret goes in config.json.
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { randomBytes } from 'node:crypto';
import { dirname, isAbsolute, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const appDir = dirname(dirname(fileURLToPath(import.meta.url)));

const DEFAULTS = {
  port: 38417,                    // TCP port the server listens on (forward this one on the router)
  bind: '0.0.0.0',
  publicHost: 'wwfallback.viosarcade.xyz', // the DNS-only name players reach it by (informational: logged at start)
  dataDir: './data',              // custom player content and the server's own salt
  tlsCert: '',                    // optional: PEM certificate (full chain) and key files; with both the server speaks wss/https.
  tlsKey: '',                     // A relative path is relative to the folder of config.json.
  maxContentBytes: 20 * 1024 ** 3, // cap on the stored custom player content (20 GiB); the least recently used goes first
  adminKey: '',                   // key for the /_admin/ routes and a remote /_status (header x-admin-key); "" = loopback only
  env: {},                        // extra variables for the worker's modules (an operator's own room log takes its settings here)
  protocols: '1',                 // the game's protocol versions this server accepts (comma-separated)
  ipSalt: '',                     // salts the hash of upload addresses; "" = a random one kept in the data folder
  maxConnections: 1500,           // WebSockets at once, all rooms
  maxConnectionsPerIp: 40,        // WebSockets at once from one address (a household behind one router)
  maxRoomsIdleMinutes: 60,        // a room with nobody connected is forgotten after this long
};

export function loadConfig() {
  const file = process.env.CONFIG ? resolve(process.env.CONFIG) : join(appDir, 'config.json');
  let fromFile = {};
  if (existsSync(file)) {
    try {
      fromFile = JSON.parse(readFileSync(file, 'utf8'));
    } catch (e) {
      throw new Error(`${file} is not valid JSON: ${e.message}`);
    }
  }
  const c = { ...DEFAULTS, ...fromFile };
  if (process.env.PORT) c.port = Number(process.env.PORT);
  if (process.env.BIND) c.bind = process.env.BIND;
  if (process.env.DATA_DIR) c.dataDir = process.env.DATA_DIR;
  if (process.env.ADMIN_KEY) c.adminKey = process.env.ADMIN_KEY;
  if (process.env.MAX_CONTENT_BYTES) c.maxContentBytes = Number(process.env.MAX_CONTENT_BYTES);
  if (!Number.isInteger(c.port) || c.port < 1 || c.port > 65535) throw new Error(`port ${c.port} is not a port`);
  if (c.env === null || typeof c.env !== 'object' || Array.isArray(c.env)) c.env = {};
  if (typeof c.adminKey !== 'string') c.adminKey = '';
  c.maxContentBytes = Number(c.maxContentBytes);
  if (!Number.isFinite(c.maxContentBytes) || c.maxContentBytes <= 0) throw new Error(`maxContentBytes ${c.maxContentBytes} is not a size in bytes`);
  if (Boolean(c.tlsCert) !== Boolean(c.tlsKey)) throw new Error('tlsCert and tlsKey go together: set both or neither');
  for (const k of ['tlsCert', 'tlsKey']) if (c[k]) c[k] = isAbsolute(c[k]) ? c[k] : resolve(dirname(file), c[k]);
  c.dataDir = isAbsolute(c.dataDir) ? c.dataDir : resolve(dirname(file), c.dataDir);
  mkdirSync(c.dataDir, { recursive: true });
  // The server's own random salt (object ids, and upload addresses unless config sets ipSalt), made once.
  const saltFile = join(c.dataDir, 'salt.txt');
  if (!existsSync(saltFile)) writeFileSync(saltFile, randomBytes(32).toString('hex'));
  c.salt = readFileSync(saltFile, 'utf8').trim();
  if (!c.ipSalt) c.ipSalt = c.salt;
  c.file = file;
  return c;
}
