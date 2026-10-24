// Tests the colour pack (kind=metadata, kind=mask and the manifest's "colors" section) of a running server AND that
// clients from before it keep working exactly as before. Start the server on a fresh state, then run the test:
//   npx wrangler dev --local --port 8811 --persist-to <dir>        node test/colors.mjs http://127.0.0.1:8811
// Needs Node 22+. The first half is the old client's flow (releases up to 2026.10.19b: archive, aaf, wave and manifest
// uploads without any colour pack; the same shape of manifest, read the way their content_validate.cpp reads it), the
// second the new one against the same server.
import { createHash } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import { checkMask, checkMetadata, checkManifest } from '../src/content.js';
const base = process.argv[2] || 'http://127.0.0.1:8811';
const sha = (b) => createHash('sha256').update(b).digest('hex');
let failures = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${ok ? '' : extra}`); if (!ok) failures++; };
const put = (hash, body, kind) => fetch(`${base}/content/${hash}${kind ? `?kind=${kind}` : ''}`, { method: 'PUT', body });
const ab = (b) => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);

// ---- builders
const be32 = (b, at, v) => b.writeUInt32BE(v >>> 0, at);
function j3d(magic, chunks = 2) {
  const b = Buffer.alloc(0x20 + chunks * 0x20);
  b.write(magic, 0, 'latin1');
  be32(b, 8, b.length); be32(b, 12, chunks);
  for (let i = 0; i < chunks; i++) { b.write('VTX1', 0x20 + i * 0x20, 'latin1'); be32(b, 0x20 + i * 0x20 + 4, 0x20); }
  return b;
}
function rarc(members) {
  let strings = 'root\0';
  const nameAt = [];
  for (const m of members) { nameAt.push(strings.length); strings += `${m.name}\0`; }
  const nodeOff = 0x20, entryOff = nodeOff + 0x10;
  const stringsOff = (entryOff + members.length * 0x14 + 31) & ~31;
  const dataOff = (stringsOff + strings.length + 31) & ~31;
  const offsets = [];
  let data = Buffer.alloc(0);
  for (const m of members) { offsets.push(data.length); data = Buffer.concat([data, m.data, Buffer.alloc((32 - (m.data.length % 32)) % 32)]); }
  const b = Buffer.alloc(0x20 + dataOff + data.length);
  b.write('RARC', 0, 'latin1');
  be32(b, 4, b.length); be32(b, 8, 0x20); be32(b, 0x0c, dataOff); be32(b, 0x10, data.length); be32(b, 0x14, data.length);
  be32(b, 0x20, 1); be32(b, 0x24, nodeOff); be32(b, 0x28, members.length); be32(b, 0x2c, entryOff); be32(b, 0x30, strings.length); be32(b, 0x34, stringsOff);
  b.write('ROOT', 0x20 + nodeOff, 'latin1');
  b.writeUInt16BE(members.length, 0x20 + nodeOff + 0x0a);
  members.forEach((m, i) => {
    const e = 0x20 + entryOff + i * 0x14;
    b.writeUInt16BE(i, e); b[e + 4] = m.type ?? 0x11; b.writeUInt16BE(nameAt[i], e + 6); be32(b, e + 8, offsets[i]); be32(b, e + 12, m.data.length);
  });
  b.write(strings, 0x20 + stringsOff, 'latin1');
  data.copy(b, 0x20 + dataOff);
  return b;
}
const goodArc = () => rarc([{ name: 'cl.bdl', data: j3d('J3D2bdl4') }, { name: 'swa.bdl', data: j3d('J3D2bdl3', 3) }]);
const aafWords = (words, pad = 0x40) => { const b = Buffer.alloc(Math.max(pad, words.length * 4)); words.forEach((w, i) => be32(b, i * 4, w)); return b; };

const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc = (buf) => { let c = 0xffffffff; for (const x of buf) c = CRC[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function chunk(type, data = Buffer.alloc(0), badCrc = false) {
  const b = Buffer.alloc(12 + data.length);
  b.writeUInt32BE(data.length, 0); b.write(type, 4, 'latin1'); data.copy(b, 8);
  b.writeUInt32BE((crc(b.subarray(4, 8 + data.length)) ^ (badCrc ? 1 : 0)) >>> 0, 8 + data.length);
  return b;
}
const SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
// An 8-bit RGBA PNG of w x h: a red square on white (like a colour mask).
function png(w = 16, h = 16, { ctype = 6, depth = 8, extra = [], raw, trailing, idatSplit = 1, filter = 0, interlace = 0, badCrc = false } = {}) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = depth; ihdr[9] = ctype; ihdr[12] = interlace;
  const bpp = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ctype] * depth;
  const row = 1 + Math.ceil((w * bpp) / 8);
  const data = raw ?? Buffer.concat(Array.from({ length: h }, (_, y) => Buffer.concat([Buffer.from([filter]), Buffer.alloc(row - 1, y < h / 2 ? 0xff : 0x80)])));
  const z = deflateSync(data);
  const parts = [];
  const step = Math.ceil(z.length / idatSplit);
  for (let i = 0; i < z.length; i += step) parts.push(chunk('IDAT', z.subarray(i, i + step), badCrc));
  const pre = ctype === 3 ? [chunk('PLTE', Buffer.from([255, 0, 0, 255, 255, 255]))] : [];
  return Buffer.concat([SIG, chunk('IHDR', ihdr), ...pre, ...extra, ...parts, chunk('IEND'), trailing ?? Buffer.alloc(0)]);
}

// ---- the validators, offline
check('a good mask passes', (await checkMask(ab(png()))) === null, await checkMask(ab(png())));
check('a good mask in several IDAT chunks passes', (await checkMask(ab(png(16, 16, { idatSplit: 3 })))) === null);
check('a palette mask passes', (await checkMask(ab(png(16, 16, { ctype: 3, depth: 1, raw: Buffer.concat(Array.from({ length: 16 }, () => Buffer.from([0, 0xaa, 0x55]))) })))) === null);
check('a greyscale mask passes', (await checkMask(ab(png(8, 8, { ctype: 0 })))) === null);
check('a mask with gAMA and pHYs passes', (await checkMask(ab(png(16, 16, { extra: [chunk('gAMA', Buffer.alloc(4)), chunk('pHYs', Buffer.alloc(9))] })))) === null);
const refuse = async (name, buf) => check(`${name} is refused`, (await checkMask(ab(buf))) !== null);
await refuse('a tEXt chunk', png(16, 16, { extra: [chunk('tEXt', Buffer.from('Comment\0hello'))] }));
await refuse('an unknown chunk', png(16, 16, { extra: [chunk('exEc', Buffer.from('MZ'))] }));
await refuse('an eXIf chunk', png(16, 16, { extra: [chunk('eXIf', Buffer.alloc(8))] }));
await refuse('a bad checksum', png(16, 16, { badCrc: true }));
await refuse('data after IEND', png(16, 16, { trailing: Buffer.from('MZ\x90\0') }));
await refuse('a PNG that is not a PNG', Buffer.concat([Buffer.from('MZ'), Buffer.alloc(100)]));
await refuse('dimensions over 2048', png(2049, 2, { raw: Buffer.concat(Array.from({ length: 2 }, () => Buffer.alloc(1 + 2049 * 4))) }));
await refuse('a filter byte of 9', png(8, 8, { filter: 9 }));
await refuse('a stream of the wrong length', png(16, 16, { raw: Buffer.alloc(100) }));
const bomb = png(2048, 2048, { raw: Buffer.alloc(2048 * (1 + 2048 * 4) + 5000000) });
await refuse('a deflate bomb (more than the header says)', bomb);
const whole = png();
let truncated = 0;
for (let n = 0; n < whole.length; n += 7) if ((await checkMask(ab(whole.subarray(0, n)))) === null) truncated++;
check('no truncated PNG passes', truncated === 0, `${truncated} passed`);
let flipped = 0;
for (let i = 8; i < whole.length; i += 5) { const c = Buffer.from(whole); c[i] ^= 0x55; if ((await checkMask(ab(c))) === null) flipped++; }
check('no PNG with one damaged byte passes', flipped === 0, `${flipped} passed`);
const meta = Buffer.from('author: someone\nHero:\n  Tunic: 5ab24a\n');
check('metadata.txt passes', checkMetadata(ab(meta)) === null);
check('metadata with a NUL is refused', checkMetadata(ab(Buffer.from('a\0b'))) !== null);
check('metadata that is not UTF-8 is refused', checkMetadata(ab(Buffer.from([0xff, 0xfe, 0x41]))) !== null);
check('metadata over 64 KB is refused', checkMetadata(ab(Buffer.alloc(65 * 1024, 0x61))) !== null);

const h0 = 'a'.repeat(64);
const colorMask = (path, size = 100) => ({ path, sha256: h0, size });
const man = (colors, extra = {}) => JSON.stringify({ v: 1, name: 'T', files: [{ path: 'Link.arc', role: 'model', sha256: h0, size: 10 }], parts: { link: true, equipment: 'model', voice: false }, colors, ...extra });
check('a manifest with a colour pack passes', checkManifest(man({ metadata: { sha256: h0, size: 50 }, masks: [colorMask('color_masks/hero_hair.png'), colorMask('color_masks/mouths/mouths3tc.1_hero skin.png')] })) === null);
for (const bad of ['../x.png', 'color_masks/../x.png', '/color_masks/a.png', 'color_masks/a.exe', 'color_masks/A.png', 'color_masks\\a.png', 'color_masks/a/b/c.png', 'other/a.png'])
  check(`colour mask path "${bad}" is refused`, checkManifest(man({ masks: [colorMask(bad)] })) !== null);
check('a duplicate mask is refused', checkManifest(man({ masks: [colorMask('color_masks/a.png'), colorMask('color_masks/a.png')] })) !== null);
check('201 masks are refused', checkManifest(man({ masks: Array.from({ length: 201 }, (_, i) => colorMask(`color_masks/m${i}.png`)) })) !== null);
check('a mask over 2 MB is refused', checkManifest(man({ masks: [colorMask('color_masks/a.png', 3 * 1024 * 1024)] })) !== null);
check('masks over 16 MB in all are refused', checkManifest(man({ masks: Array.from({ length: 10 }, (_, i) => colorMask(`color_masks/m${i}.png`, 1900000)) })) !== null);
check('a colour pack that is not an object is refused', checkManifest(man('x')) !== null);

// ---- the OLD client's flow, as releases up to 2026.10.19b do it (no colour pack anywhere)
if (!(await fetch(`${base}/content/${'0'.repeat(64)}`).then((r) => r.status).catch(() => 0))) { console.log(`no server at ${base}: only the offline checks ran`); process.exit(failures ? 1 : 0); }
const salt = Date.now().toString();
const arc = goodArc();
const aaf = aafWords([1, 0x40, 0x20, 0, 2, 0x40, 0x10, 0, 0, 0], 0x80);
const wave = Buffer.alloc(4096, 5);
check('old: HEAD of a new body is 404', (await fetch(`${base}/content/${sha(arc)}`, { method: 'HEAD' })).status === 404);
check('old: PUT Link.arc (no kind) 201', (await put(sha(arc), arc)).status === 201);
check('old: PUT JaiInit.aaf (kind=aaf) 201', (await put(sha(aaf), aaf, 'aaf')).status === 201);
check('old: PUT voice_0.aw (kind=wave) 201', (await put(sha(wave), wave, 'wave')).status === 201);
const oldManifest = Buffer.from(JSON.stringify({ v: 1, name: `Old ${salt}`, files: [
  { path: 'Link.arc', role: 'model', sha256: sha(arc), size: arc.length },
  { path: 'sound/JaiInit.aaf', role: 'voice', sha256: sha(aaf), size: aaf.length },
  { path: 'sound/voice_0.aw', role: 'voice', sha256: sha(wave), size: wave.length }], parts: { link: true, equipment: 'model', voice: true } }));
check('old: PUT manifest 201', (await put(sha(oldManifest), oldManifest, 'manifest')).status === 201);
check('old: GET manifest returns the bytes', Buffer.from(await (await fetch(`${base}/content/${sha(oldManifest)}`)).arrayBuffer()).equals(oldManifest));
check('old: GET Link.arc returns the bytes', Buffer.from(await (await fetch(`${base}/content/${sha(arc)}`)).arrayBuffer()).equals(arc));
const oldPng = png(16, 16, { extra: [chunk('tEXt', Buffer.from('a\0b'))] });
check('old: a PNG without a kind is still refused as an archive (415)', (await put(sha(oldPng), oldPng)).status === 415);
const wave2 = Buffer.alloc(4097, 6);
check('old: a bad wave bank is still 415', (await put(sha(wave2), wave2, 'wave')).status === 415);
check('old: an unknown kind is still 400', (await put(sha(Buffer.from('x')), Buffer.from('x'), 'script')).status === 400);
check('old: a manifest with a path outside the pack is still 400', (await put(sha(Buffer.from('{"v":1,"files":[{"path":"../x","sha256":"' + h0 + '","size":1}]}')), Buffer.from('{"v":1,"files":[{"path":"../x","sha256":"' + h0 + '","size":1}]}'), 'manifest')).status === 400);

// What an old client does with a NEW manifest (windows/src/content_validate.cpp ParseManifest of those releases): 64 KB at
// most, version 1, 1..64 files each with a safe path, a hex hash and a size, 20 MB in all, equipment part known; every
// other key ignored, files it has no use for skipped.
function oldClientParse(text) {
  if (text.length === 0 || text.length > 64 * 1024) return 'size';
  const m = JSON.parse(text);
  if (m.v !== 1 || !Array.isArray(m.files)) return 'version';
  if (m.files.length < 1 || m.files.length > 64) return 'files';
  let total = 0;
  for (const f of m.files) {
    if (typeof f.path !== 'string' || !/^[0-9a-f]{64}$/.test(f.sha256) || !Number.isInteger(f.size) || f.size < 0 || f.size > 20 * 1024 * 1024) return 'entry';
    total += f.size;
  }
  if (total > 20 * 1024 * 1024) return 'pack';
  if (m.parts?.equipment !== undefined && !['', 'model', 'equipment'].includes(m.parts.equipment)) return 'equipment';
  return null;
}

// ---- the NEW flow against the same server
const maskPng = (n) => png(16, 16, { raw: Buffer.concat(Array.from({ length: 16 }, (_, y) => Buffer.concat([Buffer.from([0]), Buffer.alloc(64, (y * 16 + n) & 0xff)]))) });
const masks = [];
for (let i = 0; i < 40; i++) masks.push({ path: `color_masks/${i % 2 ? 'casual' : 'hero'}_color${i}.png`, bytes: maskPng(i) });
masks.push({ path: 'color_masks/mouths/mouths3tc.1_hero_skin.png', bytes: maskPng(99) });
const metadata = Buffer.from(`author: test ${salt}\nhero:\n  - Tunic: 5ab24a\n`);
check('new: PUT metadata (kind=metadata) 201', (await put(sha(metadata), metadata, 'metadata')).status === 201);
check('new: PUT metadata again 204', (await put(sha(metadata), metadata, 'metadata')).status === 204);
for (const m of masks) {
  const r = await put(sha(m.bytes), m.bytes, 'mask');
  if (r.status !== 201) { check(`new: PUT ${m.path}`, false, r.status); break; }
}
check(`new: ${masks.length} masks stored`, (await Promise.all(masks.map((m) => fetch(`${base}/content/${sha(m.bytes)}`, { method: 'HEAD' })))).every((r) => r.status === 200));
check('new: GET a mask returns the bytes, served as a download', await (async () => {
  const r = await fetch(`${base}/content/${sha(masks[0].bytes)}`);
  return Buffer.from(await r.arrayBuffer()).equals(masks[0].bytes) && r.headers.get('content-disposition') === 'attachment' && r.headers.get('x-content-type-options') === 'nosniff';
})());
const badMask = png(16, 16, { extra: [chunk('tEXt', Buffer.from('a\0b'))] });
check('new: a PNG with a text chunk (kind=mask) is 415', (await put(sha(badMask), badMask, 'mask')).status === 415);
check('new: a PNG with a bad checksum (kind=mask) is 415', (await put(sha(png(16, 16, { badCrc: true })), png(16, 16, { badCrc: true }), 'mask')).status === 415);
check('new: an archive as kind=mask is 415', (await put(sha(rarc([{ name: 'cl.bdl', data: j3d('J3D2bdl4', 3) }])), rarc([{ name: 'cl.bdl', data: j3d('J3D2bdl4', 3) }]), 'mask')).status === 415);
const evilMeta = Buffer.from('a\0b');
check('new: metadata with a NUL (kind=metadata) is 415', (await put(sha(evilMeta), evilMeta, 'metadata')).status === 415);
check('new: a hash that does not match is 400', (await put(sha(Buffer.from('other')), masks[1].bytes, 'mask')).status === 400);
const newBody = { v: 1, name: `New ${salt}`, files: JSON.parse(oldManifest.toString()).files, parts: { link: true, equipment: 'model', voice: true },
  colors: { metadata: { sha256: sha(metadata), size: metadata.length }, masks: masks.map((m) => ({ path: m.path, sha256: sha(m.bytes), size: m.bytes.length })) } };
const newManifest = Buffer.from(JSON.stringify(newBody));
check('new: the manifest with 41 masks is under 64 KB', newManifest.length < 64 * 1024, newManifest.length);
check('new: PUT manifest with the colour pack 201', (await put(sha(newManifest), newManifest, 'manifest')).status === 201);
check('an old client reads the new manifest as it reads its own', oldClientParse(newManifest.toString()) === null && oldClientParse(oldManifest.toString()) === null, oldClientParse(newManifest.toString()));
const badColors = Buffer.from(JSON.stringify({ ...newBody, name: 'x', colors: { masks: [{ path: '../evil.png', sha256: h0, size: 5 }] } }));
check('new: a manifest with a mask path outside the pack is 400', (await put(sha(badColors), badColors, 'manifest')).status === 400);
// A new client's manifest without a colour pack is the old manifest.
const plain = Buffer.from(JSON.stringify({ ...newBody, name: 'plain', colors: undefined }));
check('new: a manifest without the section is accepted as before', (await put(sha(plain), plain, 'manifest')).status === 201);

// ---- limits: the colour pack has its own upload budget; the archives' is the old one
let maskLimited = 0;
for (let i = 0; i < 460 && !maskLimited; i++) {
  const w = png(16, 16, { raw: Buffer.concat(Array.from({ length: 16 }, (_, y) => Buffer.concat([Buffer.from([0, i & 255, i >> 8, y]), Buffer.alloc(61, 7)]))) });
  if ((await put(sha(w), w, 'mask')).status === 429) maskLimited = i;
}
check('new: mask uploads are rate limited, at their own (higher) budget', maskLimited > 300 && maskLimited < 400, maskLimited);
let archiveLimited = 0;
for (let i = 0; i < 170 && !archiveLimited; i++) {
  const b = Buffer.concat([Buffer.from('Yaz0'), Buffer.from([0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0]), Buffer.from(`rate ${i} ${Math.random()}`)]);
  if ((await put(sha(b), b)).status === 429) archiveLimited = i;
}
check('old: archive uploads are still limited at their budget, not used up by the masks', archiveLimited >= 100 && archiveLimited <= 150, archiveLimited);
console.log(failures ? `${failures} failed` : 'all passed');
process.exit(failures ? 1 : 0);
