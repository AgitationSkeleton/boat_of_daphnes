// Two scripted players for the chat portrait and message sound scenario (tools/scenarios/anchor_chat_portrait.txt): "Mira" (a red
// tunic and hat, blue hair) and "Zed" (a dark blue tunic and hat, white hair, dark skin). Each says a line and Mira finds an item
// (an ITEM_FOUND), on a schedule that starts when the game asks for the team state.
// Usage: node test/portrait_mate.mjs <ws://host:port> <room> <seconds> <team> <delay>
const [base, room, seconds, team, delay] = process.argv.slice(2);
const t0 = Date.now();
const stamp = () => String(Date.now() - t0).padStart(6);
const looks = [
  { name: 'Mira', color: '#D03030', linkColors: 'Hero.Tunic=d03030,Hero.Hat=d03030,Hero.Hair=3050e0' },
  { name: 'Zed', color: '#2A3A9A', linkColors: 'Hero.Tunic=2a3a9a,Hero.Hat=2a3a9a,Hero.Hair=f0f0f0,Hero.Skin=a06838' },
];
let started = false;
const sockets = looks.map((look, i) => {
  const ws = new WebSocket(`${base}/ws?room=${room}`);
  ws.onopen = () => {
    ws.send(JSON.stringify({ type: 'HANDSHAKE', protocol: 1,
      clientState: { name: look.name, color: look.color, teamId: team, stage: 'sea', isSaveLoaded: true, linkColors: look.linkColors } }));
    console.log(stamp(), look.name, 'connected');
  };
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.type === 'REQUEST_TEAM_STATE') {
      ws.send(JSON.stringify({ type: 'UPDATE_TEAM_STATE', state: [], seed: '', targetClientId: m.clientId }));
      if (!started) {
        started = true;
        const at = (s, f) => setTimeout(f, (Number(delay) + s) * 1000);
        at(0, () => { sockets[0].send(JSON.stringify({ type: 'CHAT', text: 'ahoy, nice boat!' })); console.log(stamp(), 'Mira CHAT'); });
        at(5, () => { sockets[0].send(JSON.stringify({ type: 'ITEM_FOUND', itemId: 0x22, name: 'Wind Waker', targetTeamId: team })); console.log(stamp(), 'Mira ITEM_FOUND'); });
        at(10, () => { sockets[1].send(JSON.stringify({ type: 'CHAT', text: 'found anything good out there? this is a longer line to see how it wraps under the portrait' })); console.log(stamp(), 'Zed CHAT'); });
      }
    }
  };
  return ws;
});
setInterval(() => sockets.forEach((ws) => ws.readyState === 1 && ws.send(JSON.stringify({ type: 'PING' }))), 30000);
setTimeout(() => process.exit(0), Number(seconds) * 1000);
