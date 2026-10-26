// boat-of-daphnes's online play server: one Cloudflare Worker.
//
//   /ws?room=<name>   a WebSocket into that room (room.js); the name is the password
//   /rooms            the rooms that asked to be listed (lobby.js)
//   /content/<hash>   custom player content by SHA-256 (content.js)
//   /health           that the server is up
//
// The protocol is PROTOCOL.md here: Ship of Harkinian's Anchor, carried over WebSockets.

import { Room } from './room.js';
import { Lobby } from './lobby.js';
import { handleContent } from './content.js';

export { Room, Lobby };

function json(body, status = 200, extra = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'content-type': 'application/json', 'access-control-allow-origin': '*', ...extra },
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    try {
      if (url.pathname === '/ws') {
        if (request.headers.get('Upgrade') !== 'websocket') return json({ error: 'expected a WebSocket upgrade' }, 426);
        const name = (url.searchParams.get('room') || '').trim();
        if (name.length < 1 || name.length > 64) return json({ error: 'room: 1 to 64 characters' }, 400);
        // Optional, off unless FALLBACK_MODE is "redirect": send players to the backup server (FALLBACK_HOST, host[:port])
        // instead of a room here. Clients that know the hint move to it; older ones see a refused connection, as if the
        // server were down. For when the operator is about to run out of plan (or wants the load elsewhere).
        if (env.FALLBACK_MODE === 'redirect' && env.FALLBACK_HOST)
          return json({ error: 'moved', fallback: String(env.FALLBACK_HOST) }, 503, { 'x-anchor-fallback': String(env.FALLBACK_HOST) });
        // The room's object comes from its name; the name never leaves the server.
        const room = env.ROOM.get(env.ROOM.idFromName(name));
        const headers = new Headers(request.headers);
        headers.set('x-room-name', name);
        // Where the player connected from (Cloudflare's data centre and country), for the room log's diagnostics.
        headers.set('x-colo', String((request.cf && request.cf.colo) || ''));
        headers.set('x-country', String((request.cf && request.cf.country) || ''));
        return room.fetch(new Request(request, { headers }));
      }
      if (url.pathname === '/rooms') {
        const lobby = env.LOBBY.get(env.LOBBY.idFromName('lobby'));
        return lobby.fetch(new Request(new URL('/list', url), { method: 'GET' }));
      }
      if (url.pathname.startsWith('/content/')) return handleContent(request, env, url);
      if (url.pathname === '/health') return json({ ok: true, time: Date.now(), protocols: env.PROTOCOLS, ...(env.FALLBACK_HOST ? { fallback: String(env.FALLBACK_HOST) } : {}) });
      if (url.pathname === '/') {
        return new Response('boat-of-daphnes online play server (Anchor for The Wind Waker).\n', {
          headers: { 'content-type': 'text/plain' },
        });
      }
      return json({ error: 'not found' }, 404);
    } catch (e) {
      return json({ error: 'server', message: String((e && e.message) || e) }, 500);
    }
  },
};
