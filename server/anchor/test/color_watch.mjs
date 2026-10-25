// A silent second player that logs what the room's other clients share as their colours: every ALL_CLIENT_STATE that
// changes someone's linkColors prints one line "<seconds since start> <name> linkColors=<spec>". For checking that a colour
// change in the game's Cosmetics Editor reaches the room at once (BR-0057b), and what a player with none set shares.
//
// Usage: node test/color_watch.mjs <ws://host:port> <room> <total seconds> [name]
const [base, room, total, name] = [process.argv[2], process.argv[3], Number(process.argv[4] || 120), process.argv[5] || 'Watcher'];
const t0 = Date.now();
const seen = new Map();
const ws = new WebSocket(`${base}/ws?room=${room}`);
ws.onopen = () => ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1, clientState: { name, teamId: 'blue', stage: 'none', room: 0, isSaveLoaded: false } }));
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.type === 'REQUEST_TEAM_STATE') ws.send(JSON.stringify({ type: 'UPDATE_TEAM_STATE', state: [], targetClientId: m.clientId }));
  if (m.type !== 'ALL_CLIENT_STATE') return;
  for (const c of m.state || []) {
    if (c.self) continue;
    const spec = c.linkColors ?? '';
    if (seen.get(c.clientId) === spec) continue;
    seen.set(c.clientId, spec);
    console.log(`${((Date.now() - t0) / 1000).toFixed(1)} ${c.name} linkColors=${spec}`);
  }
};
setTimeout(() => process.exit(0), total * 1000);
