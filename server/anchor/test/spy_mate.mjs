// A teammate that watches what the game tells the others, for the Sea Chart and Hide My Location checks.
// It logs (with the wall clock in ms) each distinct CHART_UPDATE position, each change of the game's client state (stage, room,
// hidden), every 2 s the PLAYER_UPDATE stage it sees, and TELEPORT_TO / TELEPORT_REFUSED answers. With <tp every s> it asks the
// game to teleport to it (REQUEST_TELEPORT) that often, to see the answer. It also sends its own CHART_UPDATE like a real
// client at sea, so the game's chart has a marker to draw.
// Usage: node test/spy_mate.mjs <ws://host:port> <room> <seconds> <team> [tp every s, 0 = never]
const [base, room, seconds, team, tpEvery] = process.argv.slice(2);
const ws = new WebSocket(`${base}/ws?room=${room}`);
const t0 = Date.now();
const stamp = () => String(Date.now() - t0).padStart(6);
let selfId = 0, gameId = 0, lastSea = '', lastState = '', lastPoseLog = 0, charts = 0;
ws.onopen = () => {
  ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1, clientState: { name: 'Spy', teamId: team, stage: 'sea', room: 11, isSaveLoaded: true } }));
  console.log(stamp(), 'connected');
  setInterval(() => ws.send(JSON.stringify({ type: 'CHART_UPDATE', sea: [0, -200000], angle: 0 })), 200);
  setInterval(() => ws.send(JSON.stringify({ type: 'PING' })), 30000);
  if (Number(tpEvery) > 0)
    setInterval(() => { if (gameId) { ws.send(JSON.stringify({ type: 'REQUEST_TELEPORT', targetClientId: gameId })); console.log(stamp(), 'SENT REQUEST_TELEPORT'); } }, Number(tpEvery) * 1000);
};
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.type === 'ALL_CLIENT_STATE') {
    for (const c of Object.values(m.state)) {
      if (c.self) { selfId = c.clientId; continue; }
      if (c.name === 'Spy') continue;
      gameId = c.clientId;
      const line = JSON.stringify({ stage: c.stage, room: c.room, hidden: c.hidden, online: c.online });
      if (line !== lastState) { lastState = line; console.log(stamp(), 'GAME STATE', line); }
    }
  } else if (m.type === 'CHART_UPDATE') {
    const key = JSON.stringify(m.sea);
    ++charts;
    if (key !== lastSea) { lastSea = key; console.log(stamp(), 'GOT CHART_UPDATE', key, 'angle', m.angle, '(count', charts + ')'); }
  } else if (m.type === 'PLAYER_UPDATE') {
    if (Date.now() - lastPoseLog > 2000) { lastPoseLog = Date.now(); console.log(stamp(), 'GOT PLAYER_UPDATE', m.stage, m.room, JSON.stringify(m.pos)); }
  } else if (m.type === 'REQUEST_TEAM_STATE') {
    ws.send(JSON.stringify({ type: 'UPDATE_TEAM_STATE', state: [], seed: '', targetClientId: m.clientId }));
  } else if (['TELEPORT_TO', 'TELEPORT_REFUSED'].includes(m.type)) console.log(stamp(), 'GOT', m.type, JSON.stringify(m));
};
setTimeout(() => { console.log(stamp(), 'chart updates seen:', charts); process.exit(0); }, Number(seconds) * 1000);
