// The Bunny Hood: a new item worn from the Hero's Charm slot (docs/fork/bunny-hood.md).
//
// Item 0x44 (unused in the game and in wwrando). Saved in the Hero's Charm's own byte of the collect
// table, so a vanilla save stays valid:
//   dSv_player_collect_c::mCollect[4] (0x803C4CC0): bit 0 Hero's Charm owned, bit 1 worn (the game's),
//   bit 2 Bunny Hood owned, bit 3 worn, bit 4 the Quest Status slot shows the Hood.
#ifndef BLUEWAKE_BUNNY_HOOD_H
#define BLUEWAKE_BUNNY_HOOD_H

#include "core/cpu.h"
#include "guest_coro.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLUEWAKE_BUNNY_HOOD_ITEM 0x44u
#define BLUEWAKE_BUNNY_COLLECT 0x803C4CC0u
#define BLUEWAKE_BUNNY_CHARM_OWNED 0x01u
#define BLUEWAKE_BUNNY_CHARM_WORN 0x02u
#define BLUEWAKE_BUNNY_HOOD_OWNED 0x04u
#define BLUEWAKE_BUNNY_HOOD_WORN 0x08u
#define BLUEWAKE_BUNNY_SLOT_HOOD 0x10u

// Hook ids (mods/betterww/options.txt, option bunny_hood).
#define BLUEWAKE_BUNNY_HOOK_ITEM_GET 70u  // execItemGet: r3 the item
#define BLUEWAKE_BUNNY_HOOK_SPEED 71u     // daPy_lk_c::checkNextMode, after mMaxNormalSpeed is chosen
#define BLUEWAKE_BUNNY_HOOK_MENU 72u      // dMenu_Collect_c::_move, entry
#define BLUEWAKE_BUNNY_HOOK_MENU_END 73u  // dMenu_Collect_c::_delete, entry
#define BLUEWAKE_BUNNY_HOOK_MASK 74u      // daPy_lk_c::checkMaskDraw, entry: the charm's mask on Link
#define BLUEWAKE_BUNNY_HOOK_FIRST 70u
#define BLUEWAKE_BUNNY_HOOK_LAST 74u

// The experimental switch (CVar gExperimental.BunnyHood, set by the app): off, the hood does nothing at all, as if the
// code were not there: the hooks return at once, no item rows or messages are touched, nothing is drawn or sent.
void bluewake_bunny_hood_enable(bool on);
bool bluewake_bunny_hood_enabled(void);
// Once per retrace on the game thread (game_options.c).
void bluewake_bunny_hood_retrace(CPUState* cpu);
// Hooks 70-73.
u32 bluewake_bunny_hood_hook(CPUState* cpu, u32 id);
// The dScnPly_Draw hook (puppet hook 30): true while the hood's draw has a guest call in flight, so the
// puppets' handler must not start its own. Call bluewake_bunny_hood_draw_hook when it is not.
bool bluewake_bunny_hood_draw_active(void);
u32 bluewake_bunny_hood_draw_hook(CPUState* cpu, u32 site);
// dScnPly_Delete (puppet hook 31): the hood's heaps are given back before the scene goes.
bool bluewake_bunny_hood_delete_active(void);
u32 bluewake_bunny_hood_delete_hook(CPUState* cpu, u32 site);
// Anchor: the hood on another player's head, from inside the puppet's draw (same fiber). slot is 1 + the puppet's
// slot; link_nodes are the puppet's 42 joint world matrices (3 x 4 floats each), place its position.
void bluewake_bunny_hood_draw_for(GuestCoro* co, int slot, const float* link_nodes, const float place[3]);
// Gives the Bunny Hood as the game gives an item: a field item for 0x44 is made at Link's feet and put straight into its
// get-item state, so Link holds the item up (its model) with the acquisition text, and execItemGet(0x44) then sets the owned
// bit (hook 70). Asynchronous: done by the draw hook's fiber a frame or two later. False when the hood is off or no scene is up.
bool bluewake_bunny_hood_give(void);
// The hood is worn on the local player now (Anchor sends this flag in the puppet pose).
bool bluewake_bunny_hood_worn(void);
// Team sync: the collect byte as it really is (while the pause screen shows the hood in the charm's bits, the screen's
// view is in the game's byte), and owned bits (charm 0x01, hood 0x04) added to it from a teammate. Worn bits never travel.
uint8_t bluewake_bunny_hood_real_collect(CPUState* cpu);
void bluewake_bunny_hood_add_owned(CPUState* cpu, uint8_t owned_bits);

#ifdef __cplusplus
}
#endif

#endif
