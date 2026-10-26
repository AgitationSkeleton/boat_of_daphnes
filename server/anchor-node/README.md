# Anchor fallback server (Node.js)

The Boat of Daphnes online play server (`server/anchor`, a Cloudflare Worker) running on a PC of your own, as a backup for
when the Worker is over its Cloudflare limits or down. It answers the same routes (`/ws?room=`, `/rooms`, `/content/<hash>`,
`/health`) with the same protocol (`../anchor/PROTOCOL.md`), and **it runs the Worker's own code**: `room.js`, `lobby.js`,
`content.js` and `index.js` are copied from `server/anchor/src` by `npm run sync` and run unchanged on top of a small stand-in
for the Workers runtime (`src/runtime.js`: one in-memory object per room name like a Durable Object, the WebSocket
hibernation calls, a disk key-value store for custom player content in `src/kv.js`). Only the HTTP/WebSocket listener
(`src/server.js`) is Node's own; the room log (`overlay/room_log.js`) keeps no log, as the public Worker's does.

What differs from the Worker, on purpose:

- Rooms live in memory. A restart of this server drops rooms; players reconnect by themselves and get their seat back by their
  install token (the game does this after any disconnect). Custom player content (models) is kept on disk in `data/content`
  for 30 days like the Worker's KV, and survives restarts.
- TLS (`wss://` / `https://`) on a high port with a certificate of its own that the game trusts by pin (see "Transport"
  below), not Cloudflare's TLS.
- Custom player content has a total size cap (`maxContentBytes`, 20 GiB unless set): the least recently used goes first.
- An admin key for status, purge and sweep (see "Admin routes").
- Per-address and total connection caps (`maxConnectionsPerIp`, `maxConnections`) and a heartbeat that drops dead links.

Tested with Node 24 (Node 22 LTS or newer is needed: `fetch`, `WebSocket` and `DecompressionStream` are used). Dependency:
`ws` 8.22.0, pinned in `package-lock.json`.

## Put it on the server PC (once)

The folder already holds the code, `node_modules`, `ecosystem.config.js` and this file (plus `tls/` and `config.json`, which `npm run package` never touches). On the PC (a command prompt or
PowerShell **in this folder**):

1. Node.js 22 or newer (the LTS): `node -v`. If it is missing, install it from https://nodejs.org (the LTS installer).
2. pm2, if the PC does not have it yet: `npm install -g pm2`.
3. Settings: copy `config.example.json` to `config.json`. The defaults are right (port **38417**). Edit if you want:
   - `env`: extra variables for the Worker's modules, for an operator's own room log (`ROOM_LOG_JS` names its file when
     `npm run sync` or `npm run package` runs). Keep `config.json` on this PC only: it is not part of the code.
   - `publicHost`: only shown in the log.
   - `tlsCert`, `tlsKey`: the certificate and key (PEM); relative paths are relative to the folder of `config.json`.
   - `maxContentBytes`: cap on stored custom player content (default 21474836480 = 20 GiB).
   - `adminKey`: a long random string for the admin routes (see below); empty = the routes work from this PC only.
4. Only if the `node_modules` folder is missing or the PC cannot run what is in it: `npm ci --omit=dev`.
5. Start it: `pm2 start ecosystem.config.js`, then `pm2 save`. (If pm2 already starts at boot on this PC for the other
   servers, `pm2 save` is all that is needed; otherwise set that up the way the other servers on this PC do, for example
   with `pm2-installer` or a Task Scheduler task running `pm2 resurrect` at start-up.)
6. Check it locally: open `http://127.0.0.1:38417/health`. It answers `{"ok":true,"time":...,"protocols":"1"}`.
   `pm2 logs wwanchor` shows the log (also in `logs/out.log`), `pm2 restart wwanchor` restarts it.

## Make it reachable from the internet

1. **Windows Firewall** (an administrator command prompt):
   `netsh advfirewall firewall add rule name="wwanchor" dir=in action=allow protocol=TCP localport=38417`
2. **Router**: forward **TCP 38417** (external and internal port the same) to this PC's LAN address. Give the PC a fixed
   LAN address (or a DHCP reservation) first.
3. **Cloudflare DNS** for `viosarcade.xyz` (the zone the Worker's `wwserver.viosarcade.xyz` is in): add a record
   - Type **A**, Name **`wwfallback`** (so `wwfallback.viosarcade.xyz`), IPv4 address = this network's public IP
     (https://ifconfig.me from this PC),
   - Proxy status **DNS only** (the **gray** cloud). Do not turn the proxy on: Cloudflare's proxy would not carry port 38417
     (and the point is to be off Cloudflare's limits).
   If the public IP changes now and then, update the record when it does (or use a dynamic-DNS updater for it).
4. From another network (a phone on mobile data, or a friend): `https://wwfallback.viosarcade.xyz:38417/health` must answer the
   same JSON (the browser warns about the self-signed certificate; the game does not, it trusts it by pin). A page that does not load means the forward, the firewall or the DNS record is wrong (try the public IP and
   port directly to tell which).

The game's default backup address is `wss://wwfallback.viosarcade.xyz:38417`, exactly this name and port. If you use another
name or port, players set it in Network > Anchor > Backup Server Address (and it should become the game's default in
`anchor.cpp`, `kDefaultBackupHost`, for the next release).

## Transport: TLS with a pinned certificate

A gray-cloud (DNS-only) record has no Cloudflare certificate and no public CA signs for it, so the backup uses a **self-signed
certificate** that lives in `tls/` (`cert.pem`, `key.pem`; ECDSA P-256, CN/SAN `wwfallback.viosarcade.xyz`, valid until 2036).
The server runs `wss`/`https` (TLS 1.2 or newer) when `config.json` names both (`"tlsCert": "./tls/cert.pem"`,
`"tlsKey": "./tls/key.pem"`); its start line in the log says `TLS (wss/https, TLS 1.2 or newer)`, or `NO TLS` when it is not
set. The game's client does not ask a CA: for `wwfallback.viosarcade.xyz` it accepts the connection only when the SHA-256 of the
server certificate's public key (SubjectPublicKeyInfo) equals the pin built into the game (`kDefaultBackupPin` in `anchor.cpp`),
and then ignores name and date checks. A different certificate, a man in the middle, or a changed key is refused before any
data is read. A backup address other than that host gets no pin (normal certificate validation), and a plain `host:port`
address (ws://) still works for a server on a LAN or this PC; the game logs a warning for it.

**Regenerating the certificate** (key lost or leaked, or 2036 comes): in `tls/`,

```
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout key.pem -out cert.pem -days 3650 \
  -subj "/CN=wwfallback.viosarcade.xyz" -addext "subjectAltName=DNS:wwfallback.viosarcade.xyz"
openssl x509 -in cert.pem -pubkey -noout | openssl pkey -pubin -outform DER | openssl dgst -sha256
```

The second command prints the new pin. **The game must then change `kDefaultBackupPin` in `anchor.cpp` to it and be released
again**: game builds with the old pin refuse the new certificate (they then cannot use the backup until updated). Restart the
server (`pm2 restart wwanchor`) to load the new files. Keep `key.pem` on this PC only; never copy it into the repository.

## How the game uses it

Game builds after v2026.10.24c know the backup (older builds, including v2026.10.24c, do not: they only ever talk to the
Cloudflare Worker, so they cannot play while it is down). When the Worker cannot be reached, or answers the connection with
HTTP 429 or a 5xx (what Cloudflare sends at its limits: errors 1015 / 1027 / 1102 and the like), the game moves to the backup
for the rest of that session, shows "Online play moved to the backup server", and tries the Worker again at the next
connect. Everyone in one room has to be on the same server: failover is decided per player, so when the Worker is down for
everybody they all move together within seconds; a player on an older build stays on the Worker and will not see the others.

Network > Anchor has: **Backup Server Address** (default above), **Switch to the Backup Server Automatically** (on), and
**Use Backup Server** (off; connect to the backup at once, for testing or when a room agrees to use it).

Optionally the Worker can send players to the backup *before* it runs out: set the Worker's variables `FALLBACK_HOST`
(`wwfallback.viosarcade.xyz:38417`) and `FALLBACK_MODE` (`redirect`). New `/ws` connections then get HTTP 503 and the game
moves to the backup at once (`/health` shows the `fallback` host whenever `FALLBACK_HOST` is set). It is off unless both are
set. A Worker over its quota cannot run any code, so this switch has to be turned by hand before that happens.

## Storage cap

Custom player content (`data/content`) has a cap, `maxContentBytes` (default 20 GiB). A running total is kept (scanned at
start). When an upload would take the total past the cap, the least recently used content (last read or written; a file's
modification time is its last use, so the order survives restarts) is deleted until the total is under 90% of the cap; the file
being written is never deleted. Content still expires after 30 days, swept hourly. Evictions are logged as one line with the
count and bytes, and counted in `/_status`. Uploads are rate limited per address by the Worker's own code (unchanged), so one
client cannot make the server evict in a loop. Players re-upload what they need the next time they join.

## Admin routes

Set `adminKey` in `config.json` (a long random string; restart to apply). The routes accept the header `x-admin-key: <key>` or a
request from this PC itself (loopback, no key). Without a key set they work from this PC only. From another address, a missing
or wrong key, and every unknown path under `/_admin/`, answer a plain 404 (the route is not revealed), and after 10 failed
attempts in a minute from one address even the right key gets 404 until the minute is over.

```
curl -H "x-admin-key: YOUR-KEY" https://wwfallback.viosarcade.xyz:38417/_status --insecure
curl -X POST -H "x-admin-key: YOUR-KEY" https://wwfallback.viosarcade.xyz:38417/_admin/purge-content --insecure
curl -X POST -H "x-admin-key: YOUR-KEY" https://wwfallback.viosarcade.xyz:38417/_admin/sweep --insecure
```

- `GET /_status`: uptime, rooms, lobby, connections, memory, `contentBytes` and `storage` (bytes and files stored, the cap,
  eviction counts and bytes, time of the last eviction).
- `POST /_admin/purge-content`: deletes all stored custom content; answers `{"ok":true,"bytesFreed":...}`.
- `POST /_admin/sweep`: runs the expiry sweep now; answers `{"ok":true,"removed":...}`.

(`--insecure` because the certificate is self-signed; on the PC itself use `http`/`https` on `127.0.0.1` with no key.)

## Updating

When `server/anchor/src` changes (a new Worker release), build a new folder on the dev PC and copy it over:
`cd server/anchor-node`, `npm ci`, `npm run package -- <destination>` (this keeps the destination's `config.json`, `data` and
`logs`), then `pm2 restart wwanchor` on the server PC.

## Development

```
cd server/anchor-node
npm ci
npm start                    # syncs the worker modules, then listens on config.json's port (38417)
npm test                     # the Worker's own tests (../anchor/test) against fresh servers on free local ports
npm run test:extra           # what Node adds: room list, ping, connection cap; storage cap, admin routes, TLS
```

Environment overrides: `PORT`, `BIND`, `DATA_DIR`, `ADMIN_KEY`, `MAX_CONTENT_BYTES`, `CONFIG` (path to another config
file). `/_status` (see "Admin routes") shows rooms, connections, memory and storage.
