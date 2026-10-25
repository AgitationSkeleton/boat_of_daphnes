// Team sync (online play, docs/fork/PLAN.md N3): the parts of the save a team
// shares, diffed every retrace on the game thread. What this player gained is
// queued for the network (item changes at once, even during an event); what a teammate gained is merged in at a safe point
// (in play, no event running). Merges only ever add: bits are OR-ed, maxima
// take the larger value, an item slot is filled when empty or upgraded.
//
// Synced (save offsets from g_dComIfG_gameInfo, 0x803C4C08): max life, wallet,
// max magic, sword/shield/bracelet equipped, item slots (not bottles), item-get
// bits, ammo maxima, bag-get bits, collection (swords, shields, songs, Triforce
// shards, pearls), charts and the sea map; each stage's memory (chests,
// switches, items, visited rooms, map, compass, big key, boss), including the
// live copy of the stage in play; the ocean flags; the event flags (not the
// event registers, bytes 0x79-0xFF, which are counters). Small keys are sent
// as gains and uses, since keys are spent.
#ifndef BLUEWAKE_TEAM_SYNC_H
#define BLUEWAKE_TEAM_SYNC_H

#include "core/cpu.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One change: a synced byte at `offset` (from the save's start) and its value;
// offsets 0x0F00-0x0F0F carry a small-key difference for that save table
// (signed), 0x0F10-0x0F1F a small-key count (in a team state).
typedef struct BluewakeSyncChange {
    uint16_t offset;
    uint8_t value;
} BluewakeSyncChange;

#define BLUEWAKE_SYNC_SNAPSHOT_MAX 2048

void bluewake_team_sync_retrace(CPUState* cpu);
// On while connected with sync allowed; off forgets the baseline.
void bluewake_team_sync_enable(bool on);
// This player's new changes since the last call (up to `capacity`).
unsigned bluewake_team_sync_take(BluewakeSyncChange* out, unsigned capacity);
// A teammate's changes, merged at the next safe point.
void bluewake_team_sync_give(const BluewakeSyncChange* changes, unsigned count);
// Whether a synced offset belongs to the item lane (applied and sent at once: inventory, item-get and collection bits,
// maxima, charts) rather than the settled merge (stage memories, ocean, event flags, keys).
bool bluewake_team_sync_is_item(uint16_t offset);
// The synced bytes now, as (offset, value) pairs, for the team state; 0 when
// no save is in play.
unsigned bluewake_team_sync_snapshot(BluewakeSyncChange* out, unsigned capacity);

#ifdef __cplusplus
}
#endif

#endif
