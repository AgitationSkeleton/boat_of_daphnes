// The room log: what happens in rooms (opened, joined, left, closed, refused), for whoever runs the server.
// room.js calls these for each event. This build keeps no log (as the public worker's src/room_log.js); an operator can put
// their own here (scripts/sync-worker.mjs takes it from ROOM_LOG_JS), with its settings in config.json's "env".

export function roomLog(room, event, title, fields) {}

export function roomLogWho(c, id, a) {
  return [];
}
