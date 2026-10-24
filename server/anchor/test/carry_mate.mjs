// A scripted second player for the carrying scenarios (scripts/windows/scenarios/anchor_carry_*.txt). It joins first (so it
// owns the room and sets "allowCarrying"), copies the game's Link pose once and stands still as a puppet beside Link, logs
// every CARRY_* message the game sends, and, by mode, carries the game's Link itself.
//
// Usage: node test/carry_mate.mjs <ws://host:port> <room> <seconds> <mode> [allow 0|1] [offset x] [offset z] [start at s] [end at s]
// (start and end count from the game's first pose, the seconds from this script's start.)
//
//   mode lift         stand and let the game lift this puppet (the game sends CARRY_START, then CARRY_POS while held, then
//                     CARRY_END put or CARRY_POS thrown and CARRY_END thrown); everything is logged.
//   mode lift-refuse  as lift, but this player answers the CARRY_START with CARRY_END refused: the game must let go.
//   mode carry-put    at <start> send CARRY_START to the game, hold it at my hands (CARRY_POS 10 a second, a slow sway), and at
//                     <end> put it down (CARRY_END put).
//   mode carry-throw  the same, and at <end> throw it: an arc of CARRY_POS (phase thrown) away from me (+x; MATE_ARC=dx,dz,up changes it),
//                     then CARRY_END thrown (hurt). The arc knows no walls: the game's own Link is what keeps out of them.
//   mode carry-wait   the same without an end: waits for the game's CARRY_END (break free with B) or a timeout.
//   mode carry-event  send CARRY_START at <start> (the scenario has an event running then: the game must refuse).
// The first line printed per message type is "MATE ..."; the scenario checks the game's own log, this log is evidence for the
// other end.
const [base, room, seconds, mode, allow = '1', ox = '120', oz = '0', startAt = '6', endAt = '14'] = process.argv.slice(2);
const ws = new WebSocket(`${base}/ws?room=${room}`);
const t0 = Date.now();
const log = (...a) => console.log(`MATE ${((Date.now() - t0) / 1000).toFixed(1)}s`, ...a);
let myStage = 'sea', me = 0, gameId = 0, frame = null, spot = null, started = false, ended = false, gameAt = null, tFrame = 0;

ws.onopen = () => ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1,
  roomState: { pvp: 'off', pvpScale: 1, pvpFriendlyFire: 0, playerCollision: 1, allowCarrying: Number(allow) },
  clientState: { name: 'Mate', teamId: 'blue', stage: 'sea', room: 44, isSaveLoaded: true } }));
const send = (m) => ws.send(JSON.stringify(m));

ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.type === 'ALL_CLIENT_STATE') {
    for (const c of m.state) {
      if (c.self) me = c.clientId;
      else if (c.online && !gameId) { gameId = c.clientId; log('the game is player', gameId); }
      else if (c.online && c.clientId === gameId && process.env.MATE_FOLLOW && c.stage && c.stage !== myStage) {
        // MATE_FOLLOW=1: the game changed stage (a warp): this player goes along, and takes the first pose there as its reference
        myStage = c.stage;
        frame = null;
        log('the game is in stage', c.stage, 'room', c.room, ': following');
        send({ type: 'UPDATE_CLIENT_STATE', state: { name: 'Mate', teamId: 'blue', stage: c.stage, room: c.room, isSaveLoaded: true } });
      }
    }
  }
  if (m.type === 'UPDATE_ROOM_STATE') log('room state', JSON.stringify(m.roomState), 'owner', m.ownerClientId);
  if (m.type === 'PLAYER_UPDATE' && m.pose) {
    gameAt = m.pos;
    if (!frame) {
      frame = m;
      tFrame = Date.now(); // the times below count from the game's first pose
      spot = [m.pos[0] + Number(ox), m.pos[1], m.pos[2] + Number(oz)];
      log('standing at', spot.map((v) => v.toFixed(0)).join(' '), 'stage', m.stage, 'room', m.room);
    }
  }
  if (m.type === 'CARRY_START' || m.type === 'CARRY_END') log(m.type, JSON.stringify(m));
  if (m.type === 'CARRY_START' && mode === 'lift-refuse') {
    send({ type: 'CARRY_END', targetClientId: m.clientId, reason: 'refused' });
    log('refused the lift');
  }
  if (m.type === 'CARRY_POS' && ((m.n = (globalThis.n = (globalThis.n || 0) + 1)) % 10 === 1)) log('CARRY_POS', JSON.stringify(m));
  if (m.type === 'REQUEST_TEAM_STATE') send({ type: 'UPDATE_TEAM_STATE', state: [], targetClientId: m.clientId });
};

// The puppet: the game's pose at the spot (the base position is at wire byte 6); while carrying, it stays where it is.
let feetY = 0;
setInterval(() => {
  if (!frame) return;
  const pose = Buffer.from(frame.pose, 'base64');
  for (let i = 0; i < 3; i++) pose.writeFloatLE(spot[i], 6 + 4 * i);
  send({ type: 'PLAYER_UPDATE', stage: frame.stage, room: frame.room, pos: spot, angle: frame.angle, pose: pose.toString('base64') });
}, 100);

// Carrying the game's Link.
const holdPos = (t) => [spot[0] + 10 * Math.sin(t * 2), spot[1] + 40, spot[2] + 10 * Math.cos(t * 2)];
let heldTimer = null;
function carryStart() {
  if (!frame || !gameId || started) return;
  started = true;
  const p = holdPos(0);
  send({ type: 'CARRY_START', targetClientId: gameId, pos: p, angle: 0x4000, stage: frame.stage, room: frame.room });
  log('sent CARRY_START to', gameId);
  if (mode === 'carry-event') return;
  heldTimer = setInterval(() => {
    if (ended) return;
    send({ type: 'CARRY_POS', targetClientId: gameId, pos: holdPos((Date.now() - tFrame) / 1000), angle: 0x4000, phase: 'held' });
  }, 100);
}
function carryEnd() {
  if (!started || ended || mode === 'carry-wait' || mode === 'carry-event') return;
  ended = true;
  clearInterval(heldTimer);
  const from = holdPos((Date.now() - tFrame) / 1000);
  if (mode === 'carry-put') {
    const ground = [from[0], spot[1], from[2]];
    send({ type: 'CARRY_END', targetClientId: gameId, reason: 'put', pos: ground, angle: 0x4000, hurt: 0 });
    log('put the game down at', ground.map((v) => v.toFixed(0)).join(' '));
  } else {
    // An arc away from me (+x), 14 steps of 33 ms: up 22, gravity -3 (about the game's own pot throw).
    // (MATE_ARC="dx,dz,up": the steps in x and z and the first vertical speed, to throw toward a wall: the default is +x)
    const [adx, adz, aup] = (process.env.MATE_ARC || '14,0,22').split(',').map(Number);
    let pos = [...from], vy = aup, step = 0;
    const flight = setInterval(() => {
      pos = [pos[0] + adx, pos[1] + vy, pos[2] + adz];
      vy -= 3;
      step++;
      const landed = pos[1] <= spot[1];
      if (landed) pos[1] = spot[1];
      send({ type: 'CARRY_POS', targetClientId: gameId, pos, angle: 0x4000, phase: 'thrown' });
      if (landed || step > 40) {
        clearInterval(flight);
        send({ type: 'CARRY_END', targetClientId: gameId, reason: 'thrown', pos, angle: 0x4000, hurt: 1 });
        log('threw the game; it lands at', pos.map((v) => v.toFixed(0)).join(' '));
      }
    }, 33);
  }
}
setInterval(() => {
  const t = tFrame ? (Date.now() - tFrame) / 1000 : -1;
  if (mode.startsWith('carry') && frame && t >= Number(startAt)) carryStart();
  if (started && t >= Number(endAt)) carryEnd();
}, 100);
setTimeout(() => { ws.close(); process.exit(0); }, Number(seconds) * 1000);
