// A stand-in for a Cloudflare Worker that is over its limits: every request, WebSocket upgrades included, is answered with
// HTTP <status> (default 429) and Cloudflare's error text. For trying the game's failover to the backup server.
// Usage: node test/stub_status.mjs <port> [status] [seconds to run, default until stopped]
import http from 'node:http';
const [port, status = '429', seconds] = process.argv.slice(2);
const body = 'error code: 1027';
const send = (res) => { res.writeHead(Number(status), { 'content-type': 'text/plain', 'content-length': body.length, connection: 'close' }); res.end(body); };
const server = http.createServer((req, res) => { console.log(new Date().toISOString(), req.method, req.url); send(res); });
server.on('upgrade', (req, socket) => {
  console.log(new Date().toISOString(), 'UPGRADE', req.url, '->', status);
  socket.end(`HTTP/1.1 ${status} Too Many Requests\r\ncontent-type: text/plain\r\ncontent-length: ${body.length}\r\nconnection: close\r\n\r\n${body}`);
});
server.listen(Number(port), '127.0.0.1', () => console.log(`stub answering ${status} on 127.0.0.1:${port}`));
if (seconds) setTimeout(() => process.exit(0), Number(seconds) * 1000);
