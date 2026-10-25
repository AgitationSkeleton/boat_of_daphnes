// A scripted second player for the puppet scenarios: records the game's PLAYER_UPDATE stream (with its
// "pose" joint matrices) for <record> seconds, then plays it back looped as a mate, offset from the game's Link,
// 10 times a second. With a pack it also uploads that content to the server's store and advertises its manifest in
// its client state, so the game downloads it and draws the mate as that model.
//
// Usage: node test/pose_replay.mjs <ws://host:port> <room> <record seconds> <total seconds> [pack]
//
//   pack: a wwrando model folder (Link.arc, metadata.txt, color_masks, sound): the mate has that mod's Link, equipment
//         and voice. Or parts of several folders: "link=<folder>,equip=<folder>,voice=<folder>" (any of the three; one
//         left out is the game's own). Several mods give a mixed assortment: Young Link's body with Samus's equipment.
//   env:  MATE_COLORS ("Hero.Tunic=ff0000,Hero.Hat=00ff00": the mate's Link colors, as a client shares them), MATE_COLORS2 and
//         MATE_COLORS_AFTER (seconds after the game's first pose: the colors change to MATE_COLORS2, as a player does in the Cosmetics Editor),
//         MATE_ROOM (the sea room the mate says it is in, default 44), MATE_NAME (default "Mate"), MATE_DX / MATE_DZ (offset from Link, default 150 / 0), VOICE_PITCH,
//         MATE_OFFLINE_AFTER (seconds: the mate disconnects then, for the leave scenarios),
//         MATE_LEGACY=1 (publish as an older client does: a plain manifest with only Link.arc),
//         MATE_NOCOLORPACK=1 (a current client's manifest, but without the colour pack: the model's metadata and masks are not shared),
//         MATE_WAVE=1 (the replayed pose's left arm joints swing between two angles every 1.5 s: shows the limbs follow the stream),
//         MATE_FREEZE=1 (always the last recorded pose: the puppet stands still, with one expression),
//         MATE_ATT="<arc>:<index>:<joints>:<kind>[:<height>]" (a model of a game archive held over the puppet's head, as a puppet with a carried or held object sends it),
//         MATE_LINKFACING=1 (the puppet's heading starts as the game's Link's, whatever way the recording faced; with MATE_TURN=180 it faces him),
//         MATE_TURN=<degrees> (the base is turned about the vertical: 180 faces the puppet the other way),
//         MATE_EYE="x0,y0,x1,y1" (where the pupils look: the scroll of the recorded pose's eye entries, texture units; Link's own reach 0.16 / 0.12),
//         MATE_HOLD=<kind> (the first pose of the game's with an attachment of that kind, 1 item in hand, 9 item get, 4 grab, is replayed as it was sent: what a real client sends),
//         MATE_SAVEHOLD=<file> (with MATE_HOLD: the pose is saved), MATE_LOADHOLD=<file> (that pose is replayed beside the game's Link, in another run),
//         MATE_BOMB=<seconds> (a scripted bomb: held, thrown, goes off; as a client's pose lists it, see bombChunk),
//         MATE_BOMB_LOSS=1 (with MATE_BOMB: the poses that say it went off are lost: the bomb stops being listed, and the receiver's timeout makes it go off),
//         MATE_NOEYE=1 (the eye chunk is left out, as an older client's pose has it: the pupils look straight ahead),
//         MATE_NOFACE=1 (the expression chunk is left out of the replayed pose: the puppet's face must not be the game's own Link's),
//         MATE_GEAR=1 (the replayed pose has the Hero's Sword on the back and the Hero's Shield on the arm, as a player
//         with them equipped sends them: the recorded Link may have none, and then the puppets' equipment models would not show).
import { createHash } from 'node:crypto';
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const [base, room, record, total, pack] = [process.argv[2], process.argv[3], Number(process.argv[4] || 10), Number(process.argv[5] || 90), process.argv[6]];
const http = base.replace(/^ws/, 'http');
const sha = (b) => createHash('sha256').update(b).digest('hex');

async function upload(bytes, kind) {
  const h = sha(bytes);
  if ((await fetch(`${http}/content/${h}`, { method: 'HEAD' })).status === 200) return h;
  const r = await fetch(`${http}/content/${h}${kind ? `?kind=${kind}` : ''}`, { method: 'PUT', body: bytes });
  if (r.status !== 201 && r.status !== 204) throw new Error(`upload ${h.slice(0, 12)}${kind ? ` (${kind})` : ''}: ${r.status} ${await r.text()}`);
  return h;
}

// A folder (case-insensitive file names inside, as the mods are) -> its file's path or null.
function find(folder, rel) {
  const direct = join(folder, rel);
  return existsSync(direct) ? direct : null;
}

function parseSpec(spec) {
  if (!spec.includes('=')) return { link: spec, equip: spec, voice: spec };
  const out = {};
  for (const item of spec.split(',')) {
    const [k, v] = item.split('=');
    out[k.trim()] = v.trim();
  }
  return out;
}

// A PNG with only the chunks a colour mask needs (what a client does before it uploads: the server refuses the rest).
function stripPng(png) {
  const keep = new Set(['IHDR', 'PLTE', 'tRNS', 'IDAT', 'IEND', 'gAMA', 'cHRM', 'sRGB', 'sBIT', 'pHYs', 'bKGD', 'tIME']);
  const parts = [png.subarray(0, 8)];
  for (let at = 8; at + 12 <= png.length;) {
    const len = png.readUInt32BE(at), type = png.toString('latin1', at + 4, at + 8);
    if (keep.has(type)) parts.push(png.subarray(at, at + 12 + len));
    at += 12 + len;
    if (type === 'IEND') break;
  }
  return Buffer.concat(parts);
}

// The "colors" section of the manifest (a newer client's): the model's metadata.txt and its color_masks, uploaded by kind.
async function colorPack(folder) {
  const out = { metadata: null, masks: [] };
  const meta = readFileSync(find(folder, 'metadata.txt'));
  out.metadata = { sha256: await upload(meta, 'metadata'), size: meta.length };
  const walk = (dir, rel) => {
    for (const e of readdirSync(join(dir), { withFileTypes: true })) {
      if (e.isDirectory()) walk(join(dir, e.name), `${rel}${e.name.toLowerCase()}/`);
      else if (e.name.toLowerCase().endsWith('.png')) out.masks.push({ file: join(dir, e.name), path: `${rel}${e.name.toLowerCase()}` });
    }
  };
  if (existsSync(join(folder, 'color_masks'))) walk(join(folder, 'color_masks'), 'color_masks/');
  const masks = [];
  for (const m of out.masks.slice(0, 200)) {
    const png = stripPng(readFileSync(m.file));
    try { masks.push({ path: m.path, sha256: await upload(png, 'mask'), size: png.length }); } catch (e) { console.log(`mask ${m.path} not shared: ${e.message}`); }
  }
  out.masks = masks;
  return out;
}

// The pack's manifest, as a game of this version would build it from the choice (windows/src/anchor_content.cpp).
async function publish(spec) {
  const { link, equip, voice } = parseSpec(spec);
  const list = [];
  const parts = { link: false, equipment: '', voice: false };
  let totalSize = 0;
  const add = async (path, role, file, kind) => {
    const bytes = readFileSync(file);
    totalSize += bytes.length;
    list.push({ path, role, sha256: await upload(bytes, kind), size: bytes.length });
  };
  const name = [link, equip, voice].filter(Boolean).map((f) => f.split(/[\\/]/).pop()).join('+');
  if (process.env.MATE_LEGACY) {
    // An older client: the model file and equipment archives only.
    await add('Link.arc', 'model', find(link, 'Link.arc'));
  } else {
    if (link && find(link, 'Link.arc')) {
      await add('Link.arc', 'model', find(link, 'Link.arc'));
      parts.link = true;
    }
    if (equip && find(equip, 'Link.arc')) {
      if (equip === link) parts.equipment = 'model';
      else {
        await add('equipment/Link.arc', 'equipment', find(equip, 'Link.arc'));
        parts.equipment = 'equipment';
      }
    }
    if (voice && find(voice, 'sound/JaiInit.aaf') && find(voice, 'sound/voice_0.aw')) {
      await add('sound/JaiInit.aaf', 'voice', find(voice, 'sound/JaiInit.aaf'), 'aaf');
      await add('sound/voice_0.aw', 'voice', find(voice, 'sound/voice_0.aw'), 'wave');
      if (find(voice, 'sound/GanonT_0.aw')) await add('sound/GanonT_0.aw', 'voice', find(voice, 'sound/GanonT_0.aw'), 'wave');
      parts.voice = true;
    }
  }
  const body = { v: 1, name, files: list };
  if (!process.env.MATE_LEGACY) body.parts = parts;
  if (!process.env.MATE_LEGACY && !process.env.MATE_NOCOLORPACK && parts.link && find(link, 'metadata.txt')) body.colors = await colorPack(link);
  const manifest = Buffer.from(JSON.stringify(body));
  const hash = await upload(manifest, 'manifest');
  mateParts = parts;
  console.log(`pack ${spec}: ${list.length} files, ${totalSize} bytes, parts ${JSON.stringify(parts)}, manifest ${hash}`);
  return hash;
}


// The wire (runtime/host/src/puppet.c wire_encode): 32-byte header (sword at byte 3, shield at 4, scaled-joint bits from 26),
// 42 joints of 14 bytes (+3 when scaled), then the sword's and the shield's places (7 s16 each) when they are set, then proc.
function withGear(pose) {
  if (pose[3] !== 0 || pose[4] !== 0) return pose;
  let at = 32;
  for (let i = 0; i < 42; i++) at += 14 + ((pose[26 + (i >> 3)] >> (i & 7)) & 1 ? 3 : 0);
  const place = (tx, ty, tz) => { const b = Buffer.alloc(14); b.writeInt16LE(32767, 6); b.writeInt16LE(tx * 8, 8); b.writeInt16LE(ty * 8, 10); b.writeInt16LE(tz * 8, 12); return b; };
  const out = Buffer.concat([pose.subarray(0, at), place(0, 60, -18), place(-22, 55, 6), pose.subarray(at)]);
  out[3] = 1; // the Hero's Sword
  out[4] = 1; // the Hero's Shield
  return out;
}

// The wire's extension chunks (tag, s16 length, payload) follow the joints, the sword's and shield's places and the s16 proc.
function dropChunks(pose, tags) {
  let at = 32;
  for (let i = 0; i < 42; i++) at += 14 + ((pose[26 + (i >> 3)] >> (i & 7)) & 1 ? 3 : 0);
  at += (pose[3] ? 14 : 0) + (pose[4] ? 14 : 0) + 2;
  const parts = [pose.subarray(0, at)];
  while (at + 3 <= pose.length) {
    const len = pose.readInt16LE(at + 1);
    if (!tags.includes(pose[at])) parts.push(pose.subarray(at, at + 3 + len));
    at += 3 + len;
  }
  return Buffer.concat(parts);
}

// An attachment chunk (EXT_ATTACH) holding one model of a game archive above the puppet's head, as a pose with a held or
// carried object sends it: MATE_ATT="<arc>:<resource index>:<joint count>:<kind>[:<height>]" (kind 1 item in hand, 4 grab,
// 6 throw, 9 item get). The model is not the PC's own: the archive may not be loaded there.
function attachChunk(spec) {
  const [arc, idx, nj, kind, height] = spec.split(':');
  const xf = (y) => { const b = Buffer.alloc(14); b.writeInt16LE(32767, 6); b.writeInt16LE(Math.round(y * 8), 10); return b; }; // identity, up y
  const y = Number(height || 150);
  const head = Buffer.from([Number(kind), arc.length]);
  const mid = Buffer.alloc(2); mid.writeInt16LE(Number(idx), 0);
  const body = Buffer.concat([Buffer.from([1]), head, Buffer.from(arc, 'latin1'), mid, Buffer.from([Number(nj), 0]), xf(y), ...Array.from({ length: Number(nj) }, () => xf(y))]);
  const tag = Buffer.alloc(3); tag[0] = 2; tag.writeInt16LE(body.length, 1);
  return Buffer.concat([tag, body]);
}

// The pupils (EXT_EYE, tag 6: count, then per eye the material index, texture matrix slot and the scroll x, y as s16 in 1/8192):
// MATE_EYE="x0,y0,x1,y1" sets the scroll of the eyes the recorded pose has (the game sends one entry per eye material).
function setEyes(pose, spec) {
  let at = 32;
  for (let i = 0; i < 42; i++) at += 14 + ((pose[26 + (i >> 3)] >> (i & 7)) & 1 ? 3 : 0);
  at += (pose[3] ? 14 : 0) + (pose[4] ? 14 : 0) + 2;
  const v = spec.split(',').map(Number);
  while (at + 3 <= pose.length) {
    const len = pose.readInt16LE(at + 1);
    if (pose[at] === 6) {
      for (let i = 0; i < pose[at + 3]; i++) {
        pose.writeInt16LE(Math.round((v[2 * i] ?? 0) * 8192), at + 3 + 1 + 6 * i + 2);
        pose.writeInt16LE(Math.round((v[2 * i + 1] ?? 0) * 8192), at + 3 + 1 + 6 * i + 4);
      }
      return true;
    }
    at += 3 + len;
  }
  return false;
}

// The attachment kinds a pose carries (EXT_ATTACH, tag 2: count, then per model its kind, archive name, index, joint count, scaled flag
// and its transforms): [{ kind, arc, idx, nj, scale }], scale the largest of the model's base matrix's scales when it has them.
function attachments(pose) {
  let at = 32;
  for (let i = 0; i < 42; i++) at += 14 + ((pose[26 + (i >> 3)] >> (i & 7)) & 1 ? 3 : 0);
  at += (pose[3] ? 14 : 0) + (pose[4] ? 14 : 0) + 2;
  const out = [];
  while (at + 3 <= pose.length) {
    const len = pose.readInt16LE(at + 1);
    if (pose[at] === 2) {
      let b = at + 3;
      const end = at + 3 + len;
      const count = pose[b++];
      for (let i = 0; i < count && b + 2 <= end; i++) {
        const kind = pose[b], arcLen = pose[b + 1];
        const arc = pose.toString('latin1', b + 2, b + 2 + arcLen);
        b += 2 + arcLen;
        const idx = pose.readInt16LE(b), nj = pose[b + 2], scaled = pose[b + 3];
        b += 4;
        const size = 14 + (scaled ? 3 : 0);
        out.push({ kind, arc, idx, nj, scale: scaled ? [pose[b + 14] / 64, pose[b + 15] / 64, pose[b + 16] / 64] : [1, 1, 1] });
        b += size * (1 + nj);
      }
    }
    at += 3 + len;
  }
  return out;
}

// A scripted bomb (EXT_BOMB, tag 7), as a client sends one: MATE_BOMB=<seconds per cycle> (default 8): held in the hands for the first
// quarter of the cycle, thrown (a parabola away from the puppet) for the next quarter, then it sits for a moment and goes off,
// and stays listed as exploded for two seconds. Each entry: id, kind, state (0 held, 1 free, 2 exploded), frames to go (s16),
// the world place (3 floats) and the place in the hands (3 x s16, 1/8 unit), all little endian.
function bombChunk(pose, startedAt) {
  const cycle = Number(process.env.MATE_BOMB) || 8;
  const t = ((Date.now() - startedAt) / 1000) % cycle;
  const base = [pose.readFloatLE(6), pose.readFloatLE(10), pose.readFloatLE(14)];
  const heldEnd = cycle * 0.25, flyEnd = cycle * 0.5, boom = cycle * 0.62;
  const entry = (state, rest, pos, rel) => {
    const b = Buffer.alloc(23);
    b[0] = 1; b[1] = 0; b[2] = state; b.writeInt16LE(rest, 3);
    for (let i = 0; i < 3; i++) b.writeFloatLE(pos[i], 5 + 4 * i);
    for (let i = 0; i < 3; i++) b.writeInt16LE(Math.round(rel[i] * 8), 17 + 2 * i);
    return b;
  };
  let e;
  const handRel = [20, 80, 25];
  const fuse = Math.round((boom - t) * 30);
  if (t < heldEnd) e = entry(0, fuse, [base[0] + 20, base[1] + 80, base[2] + 25], handRel);
  else if (t < flyEnd) {
    const f = (t - heldEnd) / (flyEnd - heldEnd);
    e = entry(1, fuse, [base[0] + 20 + 160 * f, base[1] + 80 + 220 * f - 300 * f * f, base[2] + 25 + 40 * f], handRel);
  } else if (t < boom) e = entry(1, fuse, [base[0] + 180, base[1] + 0, base[2] + 65], handRel);
  else if (t < boom + 2 && !process.env.MATE_BOMB_LOSS) e = entry(2, 0, [base[0] + 180, base[1] + 0, base[2] + 65], handRel);
  else return Buffer.alloc(0);
  const tag = Buffer.alloc(4);
  tag[0] = 7; tag.writeInt16LE(1 + e.length, 1); tag[3] = 1;
  return Buffer.concat([tag, e]);
}

const bombStart = Date.now();
let holdFrame = process.env.MATE_LOADHOLD ? JSON.parse(readFileSync(process.env.MATE_LOADHOLD, 'utf8')) : null;
let eyeWarned = 0;
let mateParts = null;
const content = pack ? await publish(pack) : '';
const mateName = process.env.MATE_NAME || 'Mate';
const dx = Number(process.env.MATE_DX ?? 150);
const dz = Number(process.env.MATE_DZ ?? 0);
const ws = new WebSocket(`${base}/ws?room=${room}`);
const frames = [];
let t0 = 0, at = 0, latest = null;
const state = () => ({ name: mateName, teamId: 'red', stage: 'sea', room: Number(process.env.MATE_ROOM || 44), isSaveLoaded: true, content, voicePitch: Number(process.env.VOICE_PITCH || 0),
  linkModel: mateParts && mateParts.link ? 1 : 0, linkColors: colors });
let colors = process.env.MATE_COLORS || '';
let colorsChanged = false;
if (process.env.MATE_COLORS2)
  setInterval(() => { // MATE_COLORS_AFTER seconds after the first pose from the game (so after the game has started)
    if (colorsChanged || !t0 || Date.now() - t0 < Number(process.env.MATE_COLORS_AFTER || 20) * 1000) return;
    colorsChanged = true;
    colors = process.env.MATE_COLORS2;
    console.log(`colors now ${colors}`);
    ws.send(JSON.stringify({ type: 'UPDATE_CLIENT_STATE', state: state() }));
  }, 500);
ws.onopen = () => ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1, clientState: state() }));
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.type === 'PLAYER_UPDATE' && m.pose) {
    latest = m;
    if (!t0) t0 = Date.now();
    if (Date.now() - t0 < record * 1000) frames.push(m);
    if (process.env.MATE_HOLD && !holdFrame) { // MATE_HOLD=<kind>: the first pose that holds a model of that kind (1 item in hand, 9 item get, 4 grab ...) is replayed
      const hit = attachments(Buffer.from(m.pose, 'base64')).find((a) => a.kind === Number(process.env.MATE_HOLD));
      if (hit) {
        holdFrame = m;
        console.log(`MATE_HOLD: ${JSON.stringify(hit)}`);
        if (process.env.MATE_SAVEHOLD) writeFileSync(process.env.MATE_SAVEHOLD, JSON.stringify(m)); // for a later run (MATE_LOADHOLD)
      }
    }
  }
  if (m.type === 'REQUEST_TEAM_STATE')
    ws.send(JSON.stringify({ type: 'UPDATE_TEAM_STATE', state: [], targetClientId: m.clientId }));
};
// The recording again and again, moved so it starts where the game's Link now is, dx / dz units to his side.
setInterval(() => {
  if (!frames.length || Date.now() - t0 < record * 1000 || !latest) return;
  const f = holdFrame && (process.env.MATE_HOLD || process.env.MATE_LOADHOLD) ? holdFrame : frames[process.env.MATE_FREEZE ? frames.length - 1 : at++ % frames.length]; // (frozen: the last recorded pose, when Link has settled)
  const first = frames[0];
  const pose = Buffer.from(f.pose, 'base64');
  const delta = [0, 1, 2].map((i) => (process.env.MATE_LOADHOLD ? 0 : f.pos[i] - first.pos[i] - f.pos[i]) + latest.pos[i] + (i === 0 ? dx : i === 2 ? dz : 0) - (process.env.MATE_LOADHOLD ? f.pos[i] : 0));
  for (let i = 0; i < 3; i++) pose.writeFloatLE(pose.readFloatLE(6 + 4 * i) + delta[i], 6 + 4 * i); // base position, wire byte 6
  if (process.env.MATE_LINKFACING) { // the base's heading is the game's Link's of the moment (the recording's may be another), then MATE_TURN
    const lp = Buffer.from(latest.pose, 'base64');
    for (let k = 0; k < 4; k++) pose.writeInt16LE(lp.readInt16LE(18 + 2 * k), 18 + 2 * k);
  }
  if (process.env.MATE_TURN) { // the puppet is turned this many degrees about the vertical (180: facing the camera behind Link)
    const th = Number(process.env.MATE_TURN) * Math.PI / 360, qt = [0, Math.sin(th), 0, Math.cos(th)];
    const a = [0, 1, 2, 3].map((k) => pose.readInt16LE(18 + 2 * k) / 32767);
    const r = [qt[3] * a[0] + qt[0] * a[3] + qt[1] * a[2] - qt[2] * a[1], qt[3] * a[1] - qt[0] * a[2] + qt[1] * a[3] + qt[2] * a[0],
               qt[3] * a[2] + qt[0] * a[1] - qt[1] * a[0] + qt[2] * a[3], qt[3] * a[3] - qt[0] * a[0] - qt[1] * a[1] - qt[2] * a[2]];
    for (let k = 0; k < 4; k++) pose.writeInt16LE(Math.round(r[k] * 32767), 18 + 2 * k);
  }
  if (process.env.MATE_WAVE) { // the left arm's joints swing 90 degrees about a local axis every 1.5 s: the limb animation proof
    const on = Math.floor(Date.now() / 750) % 2 === 0;
    const h = Math.SQRT1_2, qr = on ? [0, 0, h, h] : [0, 0, 0, 1];
    for (const j of [6, 7, 8]) {
      const o = 32 + 14 * j; // (no scaled joints in the idle poses)
      const a = [0, 1, 2, 3].map((k) => pose.readInt16LE(o + 2 * k) / 32767);
      const r = [a[3] * qr[0] + a[0] * qr[3] + a[1] * qr[2] - a[2] * qr[1], a[3] * qr[1] - a[0] * qr[2] + a[1] * qr[3] + a[2] * qr[0],
                 a[3] * qr[2] + a[0] * qr[1] - a[1] * qr[0] + a[2] * qr[3], a[3] * qr[3] - a[0] * qr[0] - a[1] * qr[1] - a[2] * qr[2]];
      for (let k = 0; k < 4; k++) pose.writeInt16LE(Math.round(r[k] * 32767), o + 2 * k);
    }
  }
  let sent = process.env.MATE_GEAR ? withGear(pose) : pose;
  if (process.env.MATE_ATT) sent = Buffer.concat([sent, attachChunk(process.env.MATE_ATT)]);
  if (process.env.MATE_NOFACE) sent = dropChunks(sent, [1]); // no expression in the stream (EXT_FACE)
  if (process.env.MATE_BOMB) sent = Buffer.concat([dropChunks(sent, [7]), bombChunk(sent, bombStart)]);
  if (process.env.MATE_EYE) { sent = Buffer.from(sent); if (!setEyes(sent, process.env.MATE_EYE) && !eyeWarned++) console.log('MATE_EYE: the recorded pose has no eye chunk'); }
  if (process.env.MATE_NOEYE) sent = dropChunks(sent, [6]); // no pupils in the stream (an older client's pose)
  ws.send(JSON.stringify({ type: 'PLAYER_UPDATE', stage: f.stage, room: f.room, pos: [f.pos[0] + delta[0], f.pos[1] + delta[1], f.pos[2] + delta[2]],
    angle: f.angle, pose: sent.toString('base64') }));
}, 100);
if (process.env.MATE_OFFLINE_AFTER) setTimeout(() => ws.close(), Number(process.env.MATE_OFFLINE_AFTER) * 1000);
setTimeout(() => { ws.close(); process.exit(0); }, total * 1000);
