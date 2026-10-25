# Online play protocol, version 1

This is the protocol of Ship of Harkinian's Anchor (see `docs/anchor.md`), carried over
WebSockets for The Wind Waker.

**Connection.** Connect to `wss://wwserver.viosarcade.xyz/ws?room=<name>`. The name (1 to 64
characters) is the room's password: anyone with it joins, and nobody can list a room unless its owner
lists it. `ww-global` is the public room by convention.

**Messages.** Each WebSocket text message is one JSON object with a `type`. The server stamps
`clientId` (the sender) on everything it relays. `{"type":"PING"}` is answered `{"type":"PONG"}`; send
it about every 30 seconds.

## Routing

The server handles some types itself (below). Any other type is relayed according to these fields:

- `targetClientId`: to that player only;
- `targetTeamId`: to the online players of that team;
- neither: to everyone else in the room.

With `addToQueue: true` the server also keeps the message for the team, for teammates who are not
online. Those players receive it in the next `UPDATE_TEAM_STATE` they ask for.

## Types the server handles

| Type | From | Fields | Server |
| --- | --- | --- | --- |
| `HANDSHAKE` | client, first | `protocol` (1), `clientId` (to rejoin as the same player), `clientState`, `roomState` (used if the room is new or ownerless) | gives the player an id; the first player online owns the room; answers `UPDATE_ROOM_STATE` and sends everyone `ALL_CLIENT_STATE` |
| `UPDATE_CLIENT_STATE` | client | `state`: `name`, `color`, `teamId`, `version`, `settingsHash`, `seed`, `isSaveLoaded`, `isGameComplete`, `stage`, `room`, `point` | stores the player's record, sends everyone `ALL_CLIENT_STATE` |
| `UPDATE_ROOM_STATE` | room owner | `roomState`: `syncItemsAndFlags`, `teleportMode`, `showLocationsMode`, `pvpMode`, `listed`, `title`, and the player-interaction settings `pvp` (`off`, `no_damage`, `on`), `pvpScale` (damage multiplier, 0.25-4), `pvpFriendlyFire` (0/1: hits within a team), `playerCollision` (0/1), `allowCarrying` (0/1, default 0: players may lift each other) | stores it (the interaction settings clamped to their ranges) and sends it to everyone; refused for other players |
| `UPDATE_TEAM_STATE` | client | `state` (the team's save data, see below), `targetClientId` (when answering a request) | stores it as the team's state and empties the team's queue; forwards it when targeted |
| `REQUEST_TEAM_STATE` | client | none | forwards it to a teammate who has a save loaded; if none is online, answers with the stored `state` and `queue` |
| `PLAYER_UPDATE` | client | pose (below) | sends it only to players whose `stage` is the sender's |
| `CHART_UPDATE` | client | `sea` [x, z] (world units: the player's place at sea, or the centre of the Sea Chart square of the island they are on), `angle` (the game's s16 facing) | not a server type: relayed to the whole room like any other (about 5 a second), for the other players' Sea Charts |

## Types the server sends

| Type | Fields |
| --- | --- |
| `ALL_CLIENT_STATE` | `state`: every player's record with `clientId`, `online`, and `self` (true in your own) |
| `UPDATE_ROOM_STATE` | `roomState`, `ownerClientId` |
| `ERROR` | `message` (protocol mismatch, room full, not the owner, ...) |
| `SERVER_MESSAGE` | `message` (announcements) |

## Types between players (relayed)

These carry Wind Waker's data. The client defines them.

| Type | Routing | Fields |
| --- | --- | --- |
| `SYNC_SAVE` | team, queued | `changes`: [[save offset, value], ...]: bytes of the synced save parts that this player gained (bits newly set, a larger maximum, a filled or upgraded item slot) |
| `SET_SWITCH` | team (`targetTeamId`) | `stage`, `room`, `no` (the switch), `on`, `seed` (the sender's randomizer seed, empty for the normal game; other seeds ignore it): a switch the sender's game set or cleared, applied live by teammates in the same place |
| `CHAT` | room | `text` (at most a line; control characters removed) |
| `REQUEST_TELEPORT` | one player | none |
| `TELEPORT_REFUSED` | one player | `reason` (`hidden`): the answer of a player with Hide My Location on; an older build never sends it (it just does not answer) |
| `TELEPORT_TO` | one player | `stage`, `room`, `point`, `pos`, `angle` |
| `DAMAGE_PLAYER` | one player | `damage`, `effect` (reserved: the game does not send it yet) |
| `PVP_HIT` | one player (`targetClientId` required; dropped while the room's `pvp` is `off`) | `damage` (the attacker's attack power), `hitType` (the game's attack type bits), `pos` (the attacker's place), `direction` ([x, z], from the attacker to the target) |
| `CARRY_START` | one player (`targetClientId` required; dropped while the room's `allowCarrying` is 0) | `pos` (the carrier's hands, [x, y, z]), `angle` (the carrier's s16 facing), `stage`, `room`: the sender's game has lifted the receiver's puppet like a pot and asks to hold them |
| `CARRY_POS` | one player (same rules, about 15 a second) | `pos`, `angle`, `phase` (`held`, or `thrown` while the arc flies): where the receiver's Link is held or flies; the receiver follows it with no input |
| `CARRY_END` | one player (`targetClientId` required; always relayed, so a hold can always end) | `reason` (`put`, `thrown`, `refused`, `busy`, `breakfree`, `area`, `event`, `timeout`, `left`), `pos`, `angle` (where the receiver is let go; for a throw, where the arc landed), `hurt` (1: a thrown player takes a hit when PvP is On). Either side sends it: the carrier when they put down or throw, the carried player when they refuse or break free with a button |
| `GAME_COMPLETE` | room | none (reserved: the game does not send it yet) |

**Real-time items.** What the player gets travels at once: the game diffs the item regions (item slots, item-get and bag-get bits,
collection, ammo maxima, max life, wallet, magic, charts) every frame, events included, and sends the gain as a `SYNC_SAVE` (and
`ITEM_FOUND` for the toast); the receiver applies the item regions as soon as Link can take them (no event, no scene change,
few frames in the stage) and holds them until then. Stage memories, ocean, event flags and keys keep the settled merge (a quiet
second, no event). A change that was applied from a teammate is never sent again. No new message: older clients behave as before.

**Hide My Location.** The client state carries `hidden` (bool). A receiver that knows it shows the player's area as Hidden, draws
no Sea Chart marker and does not offer teleport; the owner answers `REQUEST_TELEPORT` with `TELEPORT_REFUSED` and sends no
`CHART_UPDATE`. The stage stays in the state and the pose stream continues (players on the same stage still see each other); older
clients ignore the field and show the area.

**Team state.** `state` is the save's synced parts as [[offset, value], ...] (`runtime/host/src/team_sync.c`:
max life, wallet, max magic, equipped sword/shield/bracelet, item slots except bottles, item-get bits,
ammo maxima, bag-get bits, collection, charts and the sea map, each stage's memory, the ocean flags and the
event flags without the event registers). The receiver merges it (bits OR-ed, maxima kept, item slots filled
or upgraded) and replays `queue`. Small keys use pseudo offsets: `0x0F00 + table` carries a signed change
(a key found or used) in `SYNC_SAVE`, `0x0F10 + table` a count in a team state.

**Pose.** `PLAYER_UPDATE` carries `stage`, `room`, `pos` [x, y, z], `angle` [x, y, z], `anim` (animation ids
and frames), `equip` (sword, shield, held item), `boat` (when sailing), `outfit`, `colors`. A binary format may
replace it later; binary frames are reserved for that.

## ITEM_FOUND (client to team, no server change)

A player's game names what its own save just gained, so teammates can show "<name> found <item>".
`{"type":"ITEM_FOUND","itemId":<tww dItemNo, 0-255>,"name":"<English name>","targetTeamId":"<team>"}`.
It is relayed by the server's default case to the team's online members (the server adds `clientId`);
it is not queued (`addToQueue` is absent), so a player who joins later gets no toasts for old items.

## Carrying players (CARRY_START, CARRY_POS, CARRY_END)

A player lifts another player's puppet with the game's own lift (A near it, as with a pot) when the room's
`allowCarrying` is 1. All three types are relayed to one player (`targetClientId`); a build that does not know them
ignores them, and a build that is not carried is never sent any. The server only drops `CARRY_START` and `CARRY_POS`
while `allowCarrying` is 0 (`CARRY_END` always goes through).

1. The carrier's game sees the lift begin (the puppet's stand-in actor gets the game's carry status) and sends
   `CARRY_START` to that player.
2. The carried player's game answers by doing nothing if it can carry (the hold begins at the next `CARRY_POS`), or
   `CARRY_END` with `reason` `refused` (the room setting, an event, a scene change, the sea, the other player is
   not in the same stage and room) or `busy` (already carried). The carrier's game then lets go.
3. While held, the carrier sends `CARRY_POS` about 15 times a second. The carried player's Link is placed at `pos`
   (the carrier's hands, lowered), faces `angle`, takes no input and plays Link's crouch animation, and so does the
   puppet in every other game, because the pose stream carries it. A hold with no `CARRY_POS` for 3 seconds ends.
4. The carrier puts down or throws: `CARRY_POS` with `phase` `thrown` follows the arc (the carrier's game simulates it
   from the game's own throw speed), then `CARRY_END` (`put` or `thrown`, with `pos` the landing place). Nothing is
   damaged, except that in a room with PvP `on` a thrown player takes a small hit (`hurt` 1).
5. Either end ends the hold when the stage or room changes, an event or a cutscene starts, or the other player goes
   offline (`area`, `event`, `left`). The carried player can always press B: `CARRY_END` with `breakfree`.

## Custom content and the colour pack (HTTP, `/content/<sha256>`)

Models and voices go through the server's content store, not the WebSocket: `PUT /content/<sha256>?kind=<kind>` (the server
hashes the body, refuses it unless it is that hash, and checks it by kind), `GET` and `HEAD` by hash. Kinds: `manifest`, `archive`
(the default), `aaf`, `wave`, and since the colour pack `metadata` and `mask`. A player advertises the manifest's hash in their
client state (`content`) and the colours they chose for the model in `linkColors`.

A manifest is `{ v: 1, name, files: [{ path, role, sha256, size }], parts: { link, equipment, voice }, colors? }`. `colors` is
optional and only newer clients write it: `{ metadata: { sha256, size }, masks: [{ path: "color_masks/hero_hair.png", sha256, size }] }`,
the model's `metadata.txt` (UTF-8 text up to 64 KB, uploaded as `kind=metadata`) and its colour masks (PNG files up to 2 MB and 2048
by 2048, `kind=mask`, at most 200 listed, 16 MB in all; paths are lower case, `color_masks/` or `color_masks/mouths/`). They are not
listed in `files`, which clients before the colour pack limit to 64 entries, and those clients ignore the `colors` section. The
server checks it only when it is there. A mask is checked to the byte: chunk structure and checksums, only the chunks a mask needs
(`IHDR`, `PLTE`, `tRNS`, `IDAT`, `IEND` and a few harmless ones: `gAMA`, `cHRM`, `sRGB`, `sBIT`, `pHYs`, `bKGD`, `tIME`; anything
else, text and profile chunks included, is refused, so a sender strips them first), nothing after `IEND`, and the deflate stream
inflated, bounded, to exactly the size the header implies. Uploads of these two kinds have their own hourly budget, apart from the
archives'. `linkColors` is a list `Hero.Tunic=5ab24a,...` of the game's own Link colours; the colours of a player model follow in
the same list as `M.<Hero|Casual>.<colour name without spaces>=rrggbb`, which other clients read against the metadata they
downloaded (older clients read them as colours that no texture has).

## Pose extension chunks (binary `pose`)

After the joints, the sword's and shield's places and the `proc`, a pose carries blocks of tag (u8), length (u16 LE) and contents;
a reader skips tags it does not know. Tag 1 expression (the face textures), 2 attachments, 3 time, 4 lines, 5 sail, and since the
eyes and bombs: 6 eyes (count, then per eye material: material index, texture matrix slot, the pupil's scroll x and y as s16 in
1/8192 of a texture; a pose without it shows the pupils straight ahead) and 7 bombs (count, then per bomb: id, kind 0 bomb or 1
bomb flower, state 0 held, 1 free, 2 exploded, frames to the explosion s16, the world place as 3 floats and the place in the hands,
relative to the base, as 3 x s16 in 1/8 units; a bomb that has gone off stays listed for about two seconds). No server change: the
server relays the pose as it is.
