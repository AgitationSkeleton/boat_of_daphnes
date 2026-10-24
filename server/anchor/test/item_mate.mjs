// A teammate for the real-time item sync scenarios (tools/scenarios/anchor_item_sync.txt). It joins the team, answers the
// game's team state request with an empty state, logs every message the game sends (type, and the changes of a
// SYNC_SAVE / the item of an ITEM_FOUND, with the wall clock in ms), and sends its own finds on a schedule.
// Usage: node test/item_mate.mjs <ws://host:port> <room> <seconds> <team> <delay> <spec>... 
// Each <spec> is <offset>=<value>=<itemId>=<name>[@<extra delay s>] : a SYNC_SAVE of that one byte plus the ITEM_FOUND,
// sent <delay> seconds after the game's REQUEST_TEAM_STATE (+ the extra delay), once. Offsets may be 0x...
// BOTH=0 (env) sends the SYNC_SAVE only (an older 24b client sends both; this one keeps the old shape available).
const [base, room, seconds, team, delay, ...specs] = process.argv.slice(2);
const ws = new WebSocket(`${base}/ws?room=${room}`);
let started = false;
const t0 = Date.now();
const stamp = () => String(Date.now() - t0).padStart(6);
function sendFind(spec) {
  const [off, val, item, nameAt] = spec.split('=');
  const [name] = nameAt.split('@');
  ws.send(JSON.stringify({ type: 'SYNC_SAVE', seed: '', changes: [[Number(off), Number(val)]], targetTeamId: team, addToQueue: true }));
  if (process.env.BOTH !== '0')
    ws.send(JSON.stringify({ type: 'ITEM_FOUND', itemId: Number(item), name, targetTeamId: team }));
  console.log(stamp(), 'SENT', spec);
}
ws.onopen = () => {
  ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1,
    clientState: { name: 'Mate', teamId: team, stage: 'sea', isSaveLoaded: true } }));
  console.log(stamp(), 'connected');
};
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.type === 'REQUEST_TEAM_STATE') {
    ws.send(JSON.stringify({ type: 'UPDATE_TEAM_STATE', state: [], seed: '', targetClientId: m.clientId }));
    console.log(stamp(), 'answered a team state request');
    if (!started) {
      started = true;
      for (const spec of specs) {
        const extra = spec.includes('@') ? Number(spec.split('@')[1]) : 0;
        setTimeout(() => sendFind(spec), (Number(delay) + extra) * 1000);
      }
    }
  } else if (m.type === 'SYNC_SAVE') console.log(stamp(), 'GOT SYNC_SAVE', JSON.stringify(m.changes));
  else if (m.type === 'ITEM_FOUND') console.log(stamp(), 'GOT ITEM_FOUND', m.itemId, m.name);
  else if (['PING', 'PONG', 'ALL_CLIENT_STATE', 'UPDATE_ROOM_STATE', 'PLAYER_UPDATE', 'CHART_UPDATE'].includes(m.type)) { /* quiet */ }
  else console.log(stamp(), 'GOT', m.type, JSON.stringify(m).slice(0, 160));
};
setInterval(() => ws.send(JSON.stringify({ type: 'PING' })), 30000);
setTimeout(() => process.exit(0), Number(seconds) * 1000);
