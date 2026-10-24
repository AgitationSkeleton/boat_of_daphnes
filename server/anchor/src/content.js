// Custom player content (a player's Link model, voice and equipment files), stored by hash.
//
//   PUT  /content/<sha256>[?kind=manifest|archive|aaf|wave|metadata|mask]
//                                            a blob; the server hashes the body and refuses it unless it
//                                            is <sha256>, then checks it by kind (below).
//   GET  /content/<sha256>                   the blob (immutable: the name is the hash of the bytes)
//   HEAD /content/<sha256>                   200 if the server has it, 404 if not (so a client uploads
//                                            only what is missing)
//
// A manifest is JSON { v: 1, name, files: [{ path, role, sha256, size }], parts? } and is itself stored by its
// hash: that hash is what a player advertises in their client state.
//
// What may be stored (docs/fork/anchor-content-security.md): a manifest; a RARC archive, plain or Yaz0 (the header
// and every table entry of a plain one are checked; a Yaz0 one by its declared size, which is bounded, since the
// server does not decompress); the audio init file JaiInit.aaf (kind=aaf, its record stream is walked); a wave bank
// (kind=wave, size rules only: it has no header). Anything else is refused, so the store is not general file
// hosting on this domain. Clients check everything they download again, and never run it.
//
// Colour pack (added after the first clients shipped; they never send it and are not affected by it): the colours of a
// player model need its metadata.txt (kind=metadata: plain text) and its colour masks (kind=mask: one PNG each, red and
// white pictures). A manifest may carry them in an optional top-level section next to "files" (which older clients
// read, and which they limit to 64 entries, so the masks are not listed there):
//   "colors": { "metadata": { sha256, size }, "masks": [{ path: "color_masks/hero_hair.png", sha256, size }, ...] }
// Older clients ignore the section; this server checks it only when it is there. The PNGs are checked to the byte (chunks
// and their CRCs, the deflate stream inflated to exactly the size the header implies), and only the chunks a mask needs
// are accepted: whatever else a PNG can carry is refused, so the sender strips it before uploading.
//
// Storage: the CONTENT Workers KV namespace (values up to 25 MB; the limit here is 20 MB per file and per
// manifest). Uploads are rate-limited per client address (IP_SALT salts the hash the counter is kept under).

export const MAX_FILE = 20 * 1024 * 1024;
export const MAX_MANIFEST_TOTAL = 20 * 1024 * 1024;
const MAX_MANIFEST_BYTES = 64 * 1024;
const MAX_FILES = 64;
const UPLOADS_PER_HOUR = 150; // a pack of colour masks is dozens of small files
const BYTES_PER_HOUR = 120 * 1024 * 1024;
const HEX64 = /^[0-9a-f]{64}$/;
// Blobs expire after 30 days; a client uploads again what the server no longer has (HEAD first).
const CONTENT_TTL = 30 * 24 * 3600;

const MAX_ARCHIVE_EXPANDED = 48 * 1024 * 1024; // a Yaz0 may declare no more than this (the client's limit too)
const MAX_YAZ0_RATIO = 140; // a Yaz0 code byte makes at most 8 x 273 bytes from 17: 87 times the input at the most
const MAX_RARC_MEMBERS = 512;
const MAX_MEMBER = 8 * 1024 * 1024;
const MAX_AAF = 2 * 1024 * 1024;
const MAX_WAVE = 8 * 1024 * 1024;
const MEMBER_EXTENSIONS = new Set(['bdl', 'bmd', 'bti', 'bck', 'brk', 'btk']);
const VOICE_PATHS = new Set(['sound/JaiInit.aaf', 'sound/voice_0.aw', 'sound/GanonT_0.aw']);
const MAX_MASK = 2 * 1024 * 1024; // one colour mask PNG
const MAX_MASK_SIDE = 2048;
const MAX_MASK_RAW = 20 * 1024 * 1024; // what a mask's deflate stream may inflate to
const MAX_METADATA = 64 * 1024;
const MAX_MASKS = 200; // in one manifest (so that it stays well under its 64 KB)
const MAX_MASKS_TOTAL = 16 * 1024 * 1024;
const MASK_UPLOADS_PER_HOUR = 400; // masks and metadata have their own budget, apart from the archives'
const MASK_BYTES_PER_HOUR = 48 * 1024 * 1024;
const MASK_PATH = /^color_masks\/(mouths\/)?[a-z0-9 _.-]{1,80}\.png$/;
const ROLES = new Set(['model', 'equipment', 'voice', 'metadata', 'colors']);

const be32 = (v, at) => v.getUint32(at, false);

// A RARC archive, plain or Yaz0 (null when it is acceptable, else why not).
export function checkArchive(body) {
  if (body.byteLength < 16) return 'too short for an archive';
  const v = new DataView(body);
  const magic = new TextDecoder().decode(new Uint8Array(body, 0, 4));
  if (magic === 'Yaz0') {
    const declared = be32(v, 4);
    if (declared > MAX_ARCHIVE_EXPANDED) return 'a Yaz0 archive declares too much output';
    if (declared > body.byteLength * MAX_YAZ0_RATIO) return 'a Yaz0 archive declares more than its data can make';
    return null;
  }
  if (magic !== 'RARC') return 'only Wind Waker archives (RARC, Yaz0) are stored';
  if (body.byteLength < 0x40 || body.byteLength > MAX_ARCHIVE_EXPANDED) return 'the archive has an impossible size';
  if (be32(v, 4) > body.byteLength || be32(v, 8) !== 0x20) return 'the RARC header is wrong';
  const dataAt = 0x20 + be32(v, 0x0c);
  const dataLen = be32(v, 0x10);
  if (be32(v, 0x14) !== dataLen || be32(v, 0x18) !== 0 || be32(v, 0x1c) !== 0) return 'the RARC is not kept whole in main memory';
  if (dataAt + dataLen > body.byteLength) return 'the RARC data runs past the file';
  const nodes = be32(v, 0x20);
  const files = be32(v, 0x28);
  const strSize = be32(v, 0x30);
  const nodeAt = 0x20 + be32(v, 0x24);
  const entryAt = 0x20 + be32(v, 0x2c);
  const strAt = 0x20 + be32(v, 0x34);
  if (nodes < 1 || nodes > 64 || files < 1 || files > MAX_RARC_MEMBERS || strSize < 1 || strSize > 65536) return 'the RARC tables have impossible counts';
  if (nodeAt + nodes * 0x10 > body.byteLength || entryAt + files * 0x14 > body.byteLength || strAt + strSize > body.byteLength) return 'a RARC table runs past the file';
  const bytes = new Uint8Array(body);
  const nameAt = (offset) => {
    if (offset >= strSize) return null;
    let n = 0;
    while (offset + n < strSize && bytes[strAt + offset + n] !== 0) n++;
    if (offset + n >= strSize || n === 0 || n > 64) return null;
    return new TextDecoder('latin1').decode(bytes.subarray(strAt + offset, strAt + offset + n));
  };
  for (let i = 0; i < nodes; i++) {
    const at = nodeAt + i * 0x10;
    if (nameAt(be32(v, at + 4)) === null) return 'a RARC folder has a bad name';
    if (be32(v, at + 0x0c) + v.getUint16(at + 0x0a, false) > files) return 'a RARC folder lists files that are not there';
  }
  const seen = new Set();
  let total = 0;
  for (let i = 0; i < files; i++) {
    const at = entryAt + i * 0x14;
    const name = nameAt(v.getUint16(at + 6, false));
    if (name === null || !/^[A-Za-z0-9_.-]+$/.test(name)) return 'a RARC entry has a bad name';
    const type = bytes[at + 4];
    const offset = be32(v, at + 8);
    const length = be32(v, at + 12);
    if (type & 0x02) {
      if (offset !== 0xffffffff && offset >= nodes) return 'a RARC folder entry points outside the folder table';
      continue;
    }
    if (type & 0xe4) return 'a RARC member is compressed or not in main memory';
    if (seen.has(name.toLowerCase())) return 'a RARC member name is listed twice';
    seen.add(name.toLowerCase());
    if (offset + length > dataLen || length > MAX_MEMBER) return 'a RARC member runs past the data';
    total += length;
    if (total > MAX_ARCHIVE_EXPANDED) return 'the RARC members are too large';
    const ext = name.includes('.') ? name.slice(name.lastIndexOf('.') + 1).toLowerCase() : '';
    if (!MEMBER_EXTENSIONS.has(ext)) return 'a RARC member has a file type the game does not read from Link.arc';
    if (ext !== 'bti') {
      if (length < 0x40) return 'a RARC member is too short for a J3D file';
      const m = new TextDecoder('latin1').decode(bytes.subarray(dataAt + offset, dataAt + offset + 8));
      if (!/^J3D[12](bdl[34]|bmd[23]|bck1|brk1|btk1)$/.test(m)) return 'a RARC member is not a J3D file';
    } else if (length < 0x20) return 'a RARC member is too short for a texture';
  }
  return null;
}

// JaiInit.aaf: the record stream the game's audio init walks, every offset and size inside the file.
export function checkAaf(body) {
  if (body.byteLength < 16 || body.byteLength > MAX_AAF || body.byteLength % 4 !== 0) return 'JaiInit.aaf has an impossible size';
  const v = new DataView(body);
  const words = body.byteLength / 4;
  const w = (i) => v.getUint32(i * 4, false);
  const inside = (offset, length) => offset <= body.byteLength && length <= body.byteLength - offset;
  let r = 0;
  for (let record = 0; record < 64; record++) {
    if (r >= words) return 'JaiInit.aaf ends without its terminator';
    const type = w(r++);
    if (type === 0) return null;
    if (type === 1 || (type >= 5 && type <= 8)) {
      if (r + 3 > words) return 'a JaiInit.aaf record runs past the file';
      if (!inside(w(r), type === 8 ? (w(r + 1) & 0xfff0) + 16 : w(r + 1))) return 'a JaiInit.aaf record points outside the file';
      r += 3;
    } else if (type === 2 || type === 3) {
      let entries = 0;
      while (r < words && w(r) !== 0) {
        if (r + 3 > words || ++entries > 256) return 'a JaiInit.aaf table is too long or runs past the file';
        if (!inside(w(r), w(r + 1))) return 'a JaiInit.aaf table entry points outside the file';
        r += 3;
      }
      if (r >= words) return 'a JaiInit.aaf table is not terminated';
      r++;
    } else if (type === 4) {
      if (r + 3 > words) return 'a JaiInit.aaf record runs past the file';
      r += 3;
    } else {
      while (r < words && w(r) !== 0) r++;
      if (r >= words) return 'an unknown JaiInit.aaf record is not terminated';
      r++;
    }
  }
  return 'JaiInit.aaf has too many records';
}

// A wave bank (.aw) has no header: ADPCM frames the DSP decodes, and any bytes decode. Size and alignment only.
export function checkWave(body) {
  if (body.byteLength < 32 || body.byteLength > MAX_WAVE || body.byteLength % 32 !== 0) return 'a wave bank has an impossible size';
  return null;
}

// metadata.txt of a player model: text the client reads as a small YAML subset. Valid UTF-8, no control characters
// (tab and line breaks aside), bounded lines.
export function checkMetadata(body) {
  if (body.byteLength < 1 || body.byteLength > MAX_METADATA) return 'metadata.txt has an impossible size';
  let text;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(body);
  } catch {
    return 'metadata.txt is not UTF-8 text';
  }
  if (/[\0-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(text)) return 'metadata.txt has control characters';
  const lines = text.split(/\r\n|\n|\r/);
  if (lines.length > 4000 || lines.some((l) => l.length > 2000)) return 'metadata.txt has too many or too long lines';
  return null;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(bytes, from, to) {
  let c = 0xffffffff;
  for (let i = from; i < to; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// A colour mask: a PNG with the chunks a picture needs and nothing else (null when acceptable, else why not; async: the
// deflate stream is inflated, bounded, and must come out at exactly the size the header implies).
const PNG_ANCILLARY = new Set(['gAMA', 'cHRM', 'sRGB', 'sBIT', 'pHYs', 'bKGD', 'tIME']);
const PNG_DEPTHS = { 0: [1, 2, 4, 8, 16], 2: [8, 16], 3: [1, 2, 4, 8], 4: [8, 16], 6: [8, 16] };
const PNG_CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };
export async function checkMask(body) {
  const n = body.byteLength;
  if (n < 57 || n > MAX_MASK) return 'a mask has an impossible size';
  const b = new Uint8Array(body);
  const v = new DataView(body);
  const sig = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
  if (sig.some((x, i) => b[i] !== x)) return 'a mask is not a PNG';
  let at = 8;
  let w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
  let sawPlte = false, sawTrns = false, plteEntries = 0, idatStart = -1, idatEnded = false, ended = false, first = true;
  const idat = [];
  let idatBytes = 0;
  while (at < n) {
    if (at + 12 > n) return 'a PNG chunk runs past the file';
    const len = v.getUint32(at, false);
    if (len > n - at - 12) return 'a PNG chunk runs past the file';
    const type = String.fromCharCode(b[at + 4], b[at + 5], b[at + 6], b[at + 7]);
    if (!/^[A-Za-z]{4}$/.test(type)) return 'a PNG chunk has a bad type';
    if (crc32(b, at + 4, at + 8 + len) !== v.getUint32(at + 8 + len, false)) return 'a PNG chunk fails its checksum';
    const data = at + 8;
    if (first && type !== 'IHDR') return 'a PNG does not start with its header';
    if (ended) return 'a PNG has data after its end';
    if (type === 'IHDR') {
      if (!first || len !== 13) return 'the PNG header is wrong';
      w = v.getUint32(data, false);
      h = v.getUint32(data + 4, false);
      depth = b[data + 8];
      ctype = b[data + 9];
      interlace = b[data + 12];
      if (w < 1 || h < 1 || w > MAX_MASK_SIDE || h > MAX_MASK_SIDE) return 'a mask has impossible dimensions';
      if (!PNG_DEPTHS[ctype] || !PNG_DEPTHS[ctype].includes(depth)) return 'a PNG has an impossible colour type or depth';
      if (b[data + 10] !== 0 || b[data + 11] !== 0 || interlace > 1) return 'a PNG has an unknown compression, filter or interlace method';
    } else if (type === 'PLTE') {
      if (sawPlte || idatStart >= 0 || ctype === 0 || ctype === 4 || len === 0 || len % 3 !== 0 || len > 768) return 'a PNG palette is misplaced or malformed';
      if (ctype === 3 && len / 3 > 1 << depth) return 'a PNG palette is too large for its depth';
      sawPlte = true;
      plteEntries = len / 3;
    } else if (type === 'tRNS') {
      if (sawTrns || idatStart >= 0) return 'a PNG transparency chunk is misplaced';
      if (ctype === 0 ? len !== 2 : ctype === 2 ? len !== 6 : ctype === 3 ? len < 1 || len > plteEntries : true) return 'a PNG transparency chunk is malformed';
      if (ctype === 3 && !sawPlte) return 'a PNG transparency chunk comes before its palette';
      sawTrns = true;
    } else if (type === 'IDAT') {
      if (idatEnded) return 'a PNG image data is not contiguous';
      if (idatStart < 0) idatStart = at;
      if (len > 0) idat.push(b.subarray(data, data + len));
      idatBytes += len;
    } else if (type === 'IEND') {
      if (len !== 0 || idatStart < 0) return 'a PNG ends without image data';
      ended = true;
    } else if (PNG_ANCILLARY.has(type)) {
      if (idatStart >= 0 || len > 1024) return 'a PNG chunk is misplaced or too large';
    } else {
      return `a PNG carries a ${type} chunk, which a colour mask must not`;
    }
    if (type !== 'IDAT' && idatStart >= 0) idatEnded = true;
    first = false;
    at += 12 + len;
  }
  if (!ended) return 'a PNG has no end';
  if (ctype === 3 && !sawPlte) return 'an indexed PNG has no palette';
  // The raw size the header implies: each scanline is a filter byte and the row's bytes.
  const bits = depth * PNG_CHANNELS[ctype];
  const rows = [];
  if (interlace === 0) {
    rows.push([h, 1 + Math.ceil((w * bits) / 8)]);
  } else {
    for (const [x0, y0, dx, dy] of [[0, 0, 8, 8], [4, 0, 8, 8], [0, 4, 4, 8], [2, 0, 4, 4], [0, 2, 2, 4], [1, 0, 2, 2], [0, 1, 1, 2]]) {
      const pw = Math.ceil((w - x0) / dx);
      const ph = Math.ceil((h - y0) / dy);
      if (pw > 0 && ph > 0) rows.push([ph, 1 + Math.ceil((pw * bits) / 8)]);
    }
  }
  const expected = rows.reduce((a, [count, size]) => a + count * size, 0);
  if (expected > MAX_MASK_RAW) return 'a mask would inflate to too much';
  const z = new Uint8Array(idatBytes);
  let zat = 0;
  for (const part of idat) {
    z.set(part, zat);
    zat += part.length;
  }
  if (z.length < 6 || (z[0] & 0x0f) !== 8 || z[0] >> 4 > 7 || ((z[0] << 8) | z[1]) % 31 !== 0 || z[1] & 0x20) return 'a PNG has a bad zlib header';
  // Inflate, counting; the scanlines' filter bytes are checked as they pass.
  let seg = 0, left = rows[0][0], segLen = rows[0][1], pos = 0, total = 0;
  try {
    const reader = new Blob([z]).stream().pipeThrough(new DecompressionStream('deflate')).getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.length;
      if (total > expected) {
        await reader.cancel();
        return 'a PNG inflates to more than its header says';
      }
      for (let i = 0; i < value.length; i++) {
        if (pos === 0 && value[i] > 4) {
          await reader.cancel();
          return 'a PNG scanline has an unknown filter';
        }
        if (++pos === segLen) {
          pos = 0;
          if (--left === 0 && ++seg < rows.length) [left, segLen] = rows[seg];
        }
      }
    }
  } catch {
    return 'a PNG has a damaged deflate stream';
  }
  if (total !== expected) return 'a PNG inflates to less than its header says';
  return null;
}

const cors = { 'access-control-allow-origin': '*', 'access-control-allow-methods': 'GET, HEAD, PUT, OPTIONS', 'access-control-allow-headers': '*' };

function reply(body, status = 200, extra = {}) {
  return new Response(body, { status, headers: { ...cors, ...extra } });
}
function fail(status, message) {
  return reply(JSON.stringify({ error: message }), status, { 'content-type': 'application/json' });
}

async function sha256Hex(buffer) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', buffer));
  return Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
}

function rolePathOk(role, path) {
  if (role === 'model') return path === 'Link.arc';
  if (role === 'equipment') return /^equipment\/[A-Za-z0-9_.-]{1,60}\.arc$/.test(path);
  if (role === 'voice') return VOICE_PATHS.has(path);
  return true; // metadata and colors are never stored by this server, but an older client may list them
}

// The optional colour pack section of a manifest (see the head of this file).
function checkColorSection(c) {
  if (!c || typeof c !== 'object' || Array.isArray(c)) return 'the colour pack is malformed';
  const entry = (e, max) => e && typeof e === 'object' && HEX64.test(e.sha256) && Number.isInteger(e.size) && e.size > 0 && e.size <= max;
  if (c.metadata !== undefined && !entry(c.metadata, MAX_METADATA)) return 'the colour pack metadata entry is malformed';
  if (c.masks !== undefined) {
    if (!Array.isArray(c.masks) || c.masks.length > MAX_MASKS) return `the colour pack lists up to ${MAX_MASKS} masks`;
    const seen = new Set();
    let total = 0;
    for (const e of c.masks) {
      if (!entry(e, MAX_MASK) || typeof e.path !== 'string' || !MASK_PATH.test(e.path)) return 'a colour mask entry is malformed';
      if (seen.has(e.path)) return 'a colour mask is listed twice';
      seen.add(e.path);
      total += e.size;
    }
    if (total > MAX_MASKS_TOTAL) return 'the colour masks are over the size limit';
  }
  return null;
}

// A manifest's shape: sizes and hashes only, paths that stay inside the pack, a known role for each path.
export function checkManifest(text) {
  let m;
  try {
    m = JSON.parse(text);
  } catch {
    return 'the manifest is not JSON';
  }
  if (!m || m.v !== 1 || !Array.isArray(m.files)) return 'the manifest is not version 1';
  if (m.files.length < 1 || m.files.length > MAX_FILES) return `the manifest lists 1 to ${MAX_FILES} files`;
  if (m.parts !== undefined) {
    const p = m.parts;
    if (!p || typeof p !== 'object' || Array.isArray(p)) return 'the manifest parts are malformed';
    if (p.equipment !== undefined && !['', 'model', 'equipment'].includes(p.equipment)) return 'the equipment part is not known';
  }
  const colorsProblem = m.colors === undefined ? null : checkColorSection(m.colors);
  if (colorsProblem) return colorsProblem;
  let total = 0;
  const seen = new Set();
  for (const f of m.files) {
    if (!f || typeof f.path !== 'string' || !HEX64.test(f.sha256) || !Number.isInteger(f.size) || f.size < 0) return 'a file entry is malformed';
    if (f.path.length > 160 || f.path.startsWith('/') || f.path.includes('..') || f.path.includes('\\') || f.path.includes(':') || /[\0-\x1f\x7f]/.test(f.path)) return 'a file path is not allowed';
    if (f.path.split('/').some((part) => part === '' || part === '.')) return 'a file path is not allowed';
    if (f.role !== undefined && (typeof f.role !== 'string' || !ROLES.has(f.role))) return 'a file role is not known';
    if (f.role !== undefined && !rolePathOk(f.role, f.path)) return 'a file path does not fit its role';
    if (f.size > MAX_FILE) return 'a file is over the size limit';
    if (seen.has(f.path.toLowerCase())) return 'a path is listed twice';
    seen.add(f.path.toLowerCase());
    total += f.size;
  }
  if (total > MAX_MANIFEST_TOTAL) return `the pack is over ${MAX_MANIFEST_TOTAL >> 20} MB`;
  return null;
}

// Uploads per client address per hour (the address only as a salted hash).
async function rateLimit(request, env, bytes, bucket = 'rl', maxUploads = UPLOADS_PER_HOUR, maxBytes = BYTES_PER_HOUR) {
  const ip = request.headers.get('cf-connecting-ip') || 'local';
  const hour = Math.floor(Date.now() / 3600000);
  const key = `${bucket}:${(await sha256Hex(new TextEncoder().encode(`${env.IP_SALT || ''}|${ip}`))).slice(0, 24)}:${hour}`;
  const used = JSON.parse((await env.CONTENT.get(key)) || '{"n":0,"b":0}');
  if (used.n + 1 > maxUploads || used.b + bytes > maxBytes) return false;
  await env.CONTENT.put(key, JSON.stringify({ n: used.n + 1, b: used.b + bytes }), { expirationTtl: 7200 });
  return true;
}

export async function handleContent(request, env, url) {
  if (request.method === 'OPTIONS') return reply(null, 204);
  if (!env.CONTENT) return fail(503, 'content storage is not set up on this server');
  const hash = url.pathname.slice('/content/'.length).toLowerCase();
  if (!HEX64.test(hash)) return fail(400, 'the name is a lowercase SHA-256');
  const key = `c:${hash}`;

  if (request.method === 'HEAD') {
    const { value } = await env.CONTENT.getWithMetadata(key, 'stream');
    if (value) await value.cancel();
    return reply(null, value ? 200 : 404);
  }
  if (request.method === 'GET') {
    const body = await env.CONTENT.get(key, 'arrayBuffer');
    if (!body) return fail(404, 'unknown content');
    return reply(body, 200, {
      'content-type': 'application/octet-stream',
      'x-content-type-options': 'nosniff',
      'content-disposition': 'attachment',
      'content-security-policy': "default-src 'none'; sandbox",
      'cache-control': 'public, max-age=31536000, immutable',
    });
  }
  if (request.method === 'PUT') {
    const declared = Number(request.headers.get('content-length') || '-1');
    if (declared > MAX_FILE) return fail(413, `over ${MAX_FILE >> 20} MB`);
    if (await env.CONTENT.get(key, 'stream').then((v) => (v ? v.cancel().then(() => true) : false))) return reply(null, 204); // already here
    const body = await request.arrayBuffer();
    if (body.byteLength > MAX_FILE) return fail(413, `over ${MAX_FILE >> 20} MB`);
    if (body.byteLength === 0) return fail(400, 'empty');
    if ((await sha256Hex(body)) !== hash) return fail(400, 'the bytes do not match the hash');
    const kind = url.searchParams.get('kind') || 'archive';
    if (!['manifest', 'archive', 'aaf', 'wave', 'metadata', 'mask'].includes(kind)) return fail(400, 'kind is manifest, archive, aaf, wave, metadata or mask');
    const small = kind === 'metadata' || kind === 'mask'; // colour pack files: their own size limit and upload budget
    if (kind === 'manifest') {
      if (body.byteLength > MAX_MANIFEST_BYTES) return fail(413, 'the manifest is too large');
      const problem = checkManifest(new TextDecoder().decode(body));
      if (problem) return fail(400, problem);
    } else {
      const problem = kind === 'aaf' ? checkAaf(body) : kind === 'wave' ? checkWave(body) : kind === 'metadata' ? checkMetadata(body) : kind === 'mask' ? await checkMask(body) : checkArchive(body);
      if (problem) return fail(415, problem);
    }
    const allowed = small ? await rateLimit(request, env, body.byteLength, 'rm', MASK_UPLOADS_PER_HOUR, MASK_BYTES_PER_HOUR) : await rateLimit(request, env, body.byteLength);
    if (!allowed) return fail(429, 'too many uploads: try again later');
    await env.CONTENT.put(key, body, { expirationTtl: CONTENT_TTL });
    return reply(null, 201);
  }
  return fail(405, 'GET, HEAD or PUT');
}
