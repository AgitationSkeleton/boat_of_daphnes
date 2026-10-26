// A Workers KV stand-in on disk: one file per key under <dir>, with an expiry beside it. Only the calls
// server/anchor/src/content.js makes: get(key, type), getWithMetadata(key, 'stream'), put(key, value, { expirationTtl }).
import { createHash } from 'node:crypto';
import { createReadStream, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, statSync, utimesSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { Readable } from 'node:stream';

// Content (the "c:" keys: players' uploaded models) is capped: the least recently used goes first when a put would pass
// maxBytes, down to EVICT_TO of it. The file's mtime is its last use (set at write, moved on every successful get), so the
// order survives a restart. Small bookkeeping keys (the upload rate counters) are not content: they are neither counted
// nor evicted, since dropping a counter would reset someone's rate limit.
const EVICT_TO = 0.9;
const isContent = (name) => name.startsWith('c_');

export class DiskKV {
  constructor(dir, { maxBytes = 20 * 1024 ** 3, log = () => {} } = {}) {
    this.dir = dir;
    this.maxBytes = maxBytes;
    this.log = log;
    mkdirSync(dir, { recursive: true });
    this.index = new Map();   // file name -> { size, used (ms) } for each content file
    this.total = 0;
    this.stats = { evictedFiles: 0, evictedBytes: 0, evictions: 0, lastEvictionAt: null };
    this.scan();
  }
  // Reads the folder once: the running total and each file's last use.
  scan() {
    this.index.clear();
    this.total = 0;
    for (const name of readdirSync(this.dir)) {
      if (!isContent(name) || name.endsWith('.exp') || name.includes('.tmp')) continue;
      try {
        const st = statSync(join(this.dir, name));
        this.index.set(name, { size: st.size, used: st.mtimeMs });
        this.total += st.size;
      } catch { /* gone */ }
    }
  }
  forget(name) {
    const e = this.index.get(name);
    if (e) { this.total -= e.size; this.index.delete(name); }
  }
  touch(f) {
    const name = f.slice(this.dir.length + 1);
    const e = this.index.get(name);
    if (!e) return;
    const now = Date.now();
    e.used = now;
    try { utimesSync(f, now / 1000, now / 1000); } catch { /* gone */ }
  }
  remove(name) {
    const f = join(this.dir, name);
    rmSync(f, { force: true });
    rmSync(f + '.exp', { force: true });
    this.forget(name);
  }
  // Deletes least recently used content until the total is under EVICT_TO of the cap; never the file named keep.
  evict(keep) {
    if (this.total <= this.maxBytes) return;
    const goal = this.maxBytes * EVICT_TO;
    const order = [...this.index.entries()].filter(([n]) => n !== keep).sort((a, b) => a[1].used - b[1].used);
    let files = 0, bytes = 0;
    for (const [name, e] of order) {
      if (this.total <= goal) break;
      this.remove(name);
      files++;
      bytes += e.size;
    }
    if (files) {
      this.stats.evictions++;
      this.stats.evictedFiles += files;
      this.stats.evictedBytes += bytes;
      this.stats.lastEvictionAt = new Date().toISOString();
      this.log('info', `content over its cap: evicted ${files} least recently used files, ${bytes} bytes; ${this.total} of ${this.maxBytes} bytes kept`);
    }
  }
  // Removes all content; the bytes freed.
  purge() {
    const before = this.total;
    for (const name of [...this.index.keys()]) this.remove(name);
    return before;
  }
  file(key) {
    const safe = /^[A-Za-z0-9:_-]{1,200}$/.test(key) ? key.replace(/:/g, '_') : 'h_' + createHash('sha256').update(key).digest('hex');
    return join(this.dir, safe);
  }
  // The file of a key that exists and has not expired, else null.
  live(key) {
    const f = this.file(key);
    if (!existsSync(f)) return null;
    try {
      if (existsSync(f + '.exp') && Number(readFileSync(f + '.exp', 'utf8')) < Date.now()) {
        rmSync(f, { force: true });
        rmSync(f + '.exp', { force: true });
        this.forget(f.slice(this.dir.length + 1));
        return null;
      }
    } catch { return null; }
    return f;
  }
  async get(key, type) {
    const f = this.live(key);
    if (!f) return null;
    this.touch(f);
    if (type === 'stream') return Readable.toWeb(createReadStream(f));
    const buf = readFileSync(f);
    if (type === 'arrayBuffer') return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    return buf.toString('utf8');
  }
  async getWithMetadata(key, type) {
    return { value: await this.get(key, type), metadata: null };
  }
  async put(key, value, opts = {}) {
    const f = this.file(key);
    const data = typeof value === 'string' ? Buffer.from(value, 'utf8')
      : value instanceof ArrayBuffer ? Buffer.from(value)
      : Buffer.from(value.buffer, value.byteOffset, value.byteLength);
    const tmp = f + '.tmp' + process.pid;
    writeFileSync(tmp, data);
    renameSync(tmp, f);
    if (opts.expirationTtl) writeFileSync(f + '.exp', String(Date.now() + opts.expirationTtl * 1000));
    else rmSync(f + '.exp', { force: true });
    const name = f.slice(this.dir.length + 1);
    if (isContent(name)) {
      this.forget(name);
      this.index.set(name, { size: data.length, used: Date.now() });
      this.total += data.length;
      this.evict(name);
    }
  }
  async delete(key) {
    const f = this.file(key);
    rmSync(f, { force: true });
    rmSync(f + '.exp', { force: true });
    this.forget(f.slice(this.dir.length + 1));
  }
  // Remove expired entries (the real KV does this by itself).
  sweep() {
    let removed = 0;
    for (const name of readdirSync(this.dir)) {
      if (!name.endsWith('.exp')) continue;
      const exp = join(this.dir, name);
      try {
        if (Number(readFileSync(exp, 'utf8')) < Date.now()) {
          rmSync(exp, { force: true });
          rmSync(exp.slice(0, -4), { force: true });
          this.forget(name.slice(0, -4));
          removed++;
        }
      } catch { /* raced with a delete */ }
    }
    return removed;
  }
  // Bytes of content on disk (the running total).
  bytes() {
    return this.total;
  }
}
