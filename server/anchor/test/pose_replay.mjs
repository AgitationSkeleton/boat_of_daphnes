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
//         MATE_NAME (default "Mate"), MATE_DX / MATE_DZ (offset from Link, default 150 / 0), VOICE_PITCH,
//         MATE_OFFLINE_AFTER (seconds: the mate disconnects then, for the leave scenarios),
//         MATE_LEGACY=1 (publish as an older client does: a plain manifest with only Link.arc),
//         MATE_WAVE=1 (the replayed pose's left arm joints swing between two angles every 1.5 s: shows the limbs follow the stream),
//         MATE_GEAR=1 (the replayed pose has the Hero's Sword on the back and the Hero's Shield on the arm, as a player
//         with them equipped sends them: the recorded Link may have none, and then the puppets' equipment models would not show).
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
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

let mateParts = null;
const content = pack ? await publish(pack) : '';
const mateName = process.env.MATE_NAME || 'Mate';
const dx = Number(process.env.MATE_DX ?? 150);
const dz = Number(process.env.MATE_DZ ?? 0);
const ws = new WebSocket(`${base}/ws?room=${room}`);
const frames = [];
let t0 = 0, at = 0, latest = null;
const state = () => ({ name: mateName, teamId: 'red', stage: 'sea', room: 44, isSaveLoaded: true, content, voicePitch: Number(process.env.VOICE_PITCH || 0),
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
  }
  if (m.type === 'REQUEST_TEAM_STATE')
    ws.send(JSON.stringify({ type: 'UPDATE_TEAM_STATE', state: [], targetClientId: m.clientId }));
};
// The recording again and again, moved so it starts where the game's Link now is, dx / dz units to his side.
setInterval(() => {
  if (!frames.length || Date.now() - t0 < record * 1000 || !latest) return;
  const f = frames[at++ % frames.length];
  const first = frames[0];
  const pose = Buffer.from(f.pose, 'base64');
  const delta = [0, 1, 2].map((i) => f.pos[i] - first.pos[i] + latest.pos[i] + (i === 0 ? dx : i === 2 ? dz : 0) - f.pos[i]);
  for (let i = 0; i < 3; i++) pose.writeFloatLE(pose.readFloatLE(6 + 4 * i) + delta[i], 6 + 4 * i); // base position, wire byte 6
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
  const sent = process.env.MATE_GEAR ? withGear(pose) : pose;
  ws.send(JSON.stringify({ type: 'PLAYER_UPDATE', stage: f.stage, room: f.room, pos: [f.pos[0] + delta[0], f.pos[1] + delta[1], f.pos[2] + delta[2]],
    angle: f.angle, pose: sent.toString('base64') }));
}, 100);
if (process.env.MATE_OFFLINE_AFTER) setTimeout(() => ws.close(), Number(process.env.MATE_OFFLINE_AFTER) * 1000);
setTimeout(() => { ws.close(); process.exit(0); }, total * 1000);
