// A teammate for the game: joins team red, prints the save changes it receives, and after
// a delay gives the game an item (the sail, slot 1 = save offset 0x3D, item 0x78).
// Usage: node test/teammate.mjs <ws://host:port> <room> <seconds> [delay seconds, default 20]
const [base, room, seconds, delay] = [process.argv[2], process.argv[3], Number(process.argv[4] || 60), Number(process.argv[5] || 20)];
const ws = new WebSocket(`${base}/ws?room=${room}`);
ws.onopen = () => {
  ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1, clientState: { name: 'Mate', teamId: 'red', stage: 'sea', isSaveLoaded: true } }));
  setTimeout(() => {
    ws.send(JSON.stringify({ type: 'SYNC_SAVE', changes: [[0x3D, 0x78]], targetTeamId: 'red', addToQueue: true }));
    console.log('sent the sail');
  }, delay * 1000);
};
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.type === 'SYNC_SAVE') console.log('SYNC_SAVE', JSON.stringify(m.changes));
  if (m.type === 'REQUEST_TEAM_STATE') {
    ws.send(JSON.stringify({ type: 'UPDATE_TEAM_STATE', state: [], targetClientId: m.clientId }));
    console.log('answered a team state request');
  }
};
setTimeout(() => { ws.close(); process.exit(0); }, seconds * 1000);
