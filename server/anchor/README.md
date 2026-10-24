# Online play server (Anchor)

Boat of Daphnes' server for online play (Anchor, as in Ship of Harkinian, for The Wind Waker). It is one Cloudflare
Worker:

- **Rooms:** each room is a Durable Object (`src/room.js`), using WebSocket hibernation, so an idle room costs nothing.
- **Room list:** a Lobby Durable Object (`src/lobby.js`) lists the rooms whose owners ask to be listed.
- **Custom player content:** models and voices players share are kept by hash in a KV namespace (`src/content.js`). A manifest may also carry a colour pack (the model's `metadata.txt` and its colour mask PNGs, uploaded as `kind=metadata` / `kind=mask` and listed in a `colors` section beside `files`): older clients ignore it, and the server checks it only when it is present (`test/colors.mjs` runs an old client's flow and the new one against the same server).
- **Protocol:** see `PROTOCOL.md`.

The game connects to the project's server by default; Settings > Network > Anchor > Host points it at another one.

## Run it locally

```
npm install
npm run dev          # http://127.0.0.1:8787 (ws://127.0.0.1:8787/ws?room=test)
npm test             # with `npm run dev` running: players in a room, routing, team state, PvP settings
```

Then set the game's Anchor host to `127.0.0.1:8787`. The scripts in `test/` are scripted players for trying features
against a local server (a puppet replaying a pose, a teammate sending items, a PvP opponent, and so on); each says how
to run it at its top.

## Deploy your own

```
npx wrangler login
npx wrangler kv namespace create CONTENT     # put the id in wrangler.jsonc
npm run deploy
```

Add a `routes` entry in `wrangler.jsonc` to serve it on your own domain. The first deploy creates the Durable Object
classes (migration `v1`); later changes are just `npm run deploy`.

Optional secret: `IP_SALT` salts the hash the content upload rate limit is kept under (`wrangler secret put IP_SALT`).

## Limits

- 32 players a room.
- 2,000 stored packets a team.
- 64 KB a message.
- A room's state (players, owner, settings, team states and queues) lives in its Durable Object's storage until the
  room is unused.
