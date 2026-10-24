// Three players against a running server (npm run dev): handshake, ids and ownership,
// relaying to the room, a team and one player, the team queue and state, stage-filtered pose.
// Usage: node test/room.mjs [ws://127.0.0.1:8787]

const base = process.argv[2] || 'ws://127.0.0.1:8787';
const room = `test-${Date.now()}`;
let failures = 0;

function check(ok, what) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) failures++;
}

function player(name, teamId, stage) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`${base}/ws?room=${room}`);
    const p = { ws, name, inbox: [], id: 0 };
    ws.onmessage = (e) => {
      const msg = JSON.parse(e.data);
      p.inbox.push(msg);
      if (msg.type === 'ALL_CLIENT_STATE') {
        const self = msg.state.find((c) => c.self);
        if (self) p.id = self.clientId;
      }
    };
    ws.onerror = reject;
    ws.onopen = () => {
      ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1, clientState: { name, teamId, stage, isSaveLoaded: true } }));
      resolve(p);
    };
  });
}

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const got = (p, type, pred = () => true) => p.inbox.some((m) => m.type === type && pred(m));
// waits until cond() holds (polling every 50 ms) or ms pass: slow CI runners can take longer than a fixed wait
const until = async (cond, ms = 3000) => { for (let t = 0; t < ms && !cond(); t += 50) await wait(50); };

const a = await player('Ann', 'red', 'sea');
await wait(300);
const b = await player('Bo', 'red', 'sea');
const c = await player('Cy', 'blue', 'Adanmae');
await wait(500);

check(a.id && b.id && c.id && new Set([a.id, b.id, c.id]).size === 3, 'three distinct ids');
check(got(a, 'UPDATE_ROOM_STATE', (m) => m.ownerClientId === a.id), 'the first player owns the room');
check(got(c, 'ALL_CLIENT_STATE', (m) => m.state.length === 3), 'everyone sees three players');

a.ws.send(JSON.stringify({ type: 'GIVE_ITEM', item: 0x20, targetTeamId: 'red', addToQueue: true }));
await wait(300);
check(got(b, 'GIVE_ITEM', (m) => m.clientId === a.id), 'team message reaches a teammate');
check(!got(c, 'GIVE_ITEM'), 'team message does not reach the other team');

c.ws.send(JSON.stringify({ type: 'SERVER_PING_TEST', targetClientId: a.id }));
await wait(300);
check(got(a, 'SERVER_PING_TEST') && !got(b, 'SERVER_PING_TEST'), 'targeted message reaches only its player');

a.ws.send(JSON.stringify({ type: 'PLAYER_UPDATE', pos: [0, 0, 0] }));
await until(() => got(b, 'PLAYER_UPDATE'));
await wait(300);  // (and time for a wrong delivery to the other stage to show)
check(got(b, 'PLAYER_UPDATE') && !got(c, 'PLAYER_UPDATE'), 'pose goes only to the same stage');

c.ws.send(JSON.stringify({ type: 'UPDATE_ROOM_STATE', roomState: { pvpMode: 1 } }));
await wait(300);
check(got(c, 'ERROR', (m) => /owner/.test(m.message)), 'only the owner changes the room');

// Player interaction: PvP hits are relayed only to their target and only while the room's PvP is on; the settings are clamped.
c.inbox.length = 0;
a.inbox.length = 0;
b.inbox.length = 0;
b.ws.send(JSON.stringify({ type: 'PVP_HIT', targetClientId: a.id, damage: 2 }));
await wait(300);
check(!got(a, 'PVP_HIT'), 'a hit is dropped while PvP is off (the default)');
a.ws.send(JSON.stringify({ type: 'UPDATE_ROOM_STATE', roomState: { pvp: 'no_damage', pvpScale: 99, pvpFriendlyFire: 'yes', playerCollision: 0 } }));
await wait(300);
check(got(b, 'UPDATE_ROOM_STATE', (m) => m.roomState.pvp === 'no_damage' && m.roomState.pvpScale === 4 &&
      m.roomState.pvpFriendlyFire === 1 && m.roomState.playerCollision === 0), 'the interaction settings are stored, clamped to their ranges');
a.ws.send(JSON.stringify({ type: 'UPDATE_ROOM_STATE', roomState: { pvp: 'bogus' } }));
await wait(300);
check(b.inbox.filter((m) => m.type === 'UPDATE_ROOM_STATE').pop().roomState.pvp === 'off', 'an unknown PvP mode becomes off');
a.ws.send(JSON.stringify({ type: 'UPDATE_ROOM_STATE', roomState: { pvp: 'on' } }));
await wait(200);
b.ws.send(JSON.stringify({ type: 'PVP_HIT', targetClientId: a.id, damage: 2, hitType: 2, pos: [0, 0, 0], direction: [0, 1] }));
b.ws.send(JSON.stringify({ type: 'PVP_HIT', damage: 2 }));
await wait(300);
check(got(a, 'PVP_HIT', (m) => m.clientId === b.id && m.damage === 2), 'a hit reaches its target while PvP is on');
check(a.inbox.filter((m) => m.type === 'PVP_HIT').length === 1 && !got(c, 'PVP_HIT'), 'a hit without a target goes nowhere, and no one else gets it');

// Team state with nobody to answer: Bo leaves, Ann leaves; a new red player asks.
b.ws.close();
a.ws.close();
await wait(500);
const d = await player('Di', 'red', 'sea');
await wait(300);
d.ws.send(JSON.stringify({ type: 'REQUEST_TEAM_STATE' }));
await wait(400);
check(got(d, 'UPDATE_TEAM_STATE', (m) => Array.isArray(m.queue) && m.queue.length === 1),
      'the server answers a team request from storage, with the queued item');
check(got(d, 'UPDATE_ROOM_STATE', (m) => m.ownerClientId === c.id || m.ownerClientId === d.id), 'ownership moves on');

c.ws.close();
d.ws.close();
console.log(failures ? `${failures} failed` : 'all passed');
process.exit(failures ? 1 : 0);
