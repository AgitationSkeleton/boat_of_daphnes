// Other online players drawn as Link models (docs/fork/research/anchor-puppets.md).
//
// Each player's Link is a second J3DModel made from the resident Link archive's
// body model. It is not animated here: the sender reads the joint matrices of
// its own Link (the game has calculated them for this frame), sends them
// compactly, and the receiver writes the interpolated matrices into the
// puppet's model and has the game finish and draw it. So every action Link has
// (walking, rolling, swimming, an item held out) shows, with no animation
// state to follow, and none of the player actor's callbacks and blend state
// (which the one Link model data shares) is touched.
//
// Threads: the hook runs on the game thread, the rest on the app's. The
// remote table is behind a lock.
#ifndef BLUEWAKE_PUPPET_H
#define BLUEWAKE_PUPPET_H

#include "core/cpu.h"
#include "guest_coro.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLUEWAKE_PUPPET_HOOK_DRAW 30u   // dScnPly_Draw, after the process draw loop
#define BLUEWAKE_PUPPET_HOOK_DELETE 31u // dScnPly_Delete, its first instruction
#define BLUEWAKE_PUPPET_MAX_WIRE 2048u

u32 bluewake_puppet_hook(CPUState* cpu, u32 id);
// The draw hook's guest call is in flight: no other draw-hook user may start one.
bool bluewake_puppet_draw_active(void);

// The app: whether to read Link's pose each frame (while online) and whether to
// draw the remotes' puppets.
void bluewake_puppet_enable(bool capture, bool draw);
// Link's latest pose as wire bytes (bluewake_puppet_wire in puppet.c). Returns
// the length, 0 if there is none; *seq changes with each new capture.
size_t bluewake_puppet_local_pose(uint8_t* out, size_t capacity, uint32_t* seq);
// A remote player's pose, as received.
void bluewake_puppet_remote_update(int id, const uint8_t* wire, size_t length);
// The player's own body model (the cl.bdl of a validated custom Link.arc): used the next time that
// player's puppet is made (the next scene, or when it first shows). Empty clears it: Link's own.
void bluewake_puppet_remote_model(int id, const uint8_t* bdl, size_t length);
// The player's own equipment models (BDL files) by slot: 0 swgripa (the sword), 1 swgripms (the Master Sword), 2 sha (the
// shield), 3 shms (the Mirror Shield), 4 swa (the sword's blade), 5 swms (the Master Sword's blade), 6 hands, 7 katsura (the hair of the Outset clothes). Used when that
// player's puppet is made (a puppet already made is made again). Empty clears the slot: the game's own models of the local
// Link. A PC whose own Link is a mod's sets the game's models here for players with none of their own.
#define BLUEWAKE_PUPPET_EQUIP_SLOTS 8
void bluewake_puppet_remote_equipment(int id, int slot, const uint8_t* bdl, size_t length);
// The owner's voice: an id ("" the game's) and whether the local audio has that voice's bank loaded (the game reads its voice
// files once at start, so only the local voice can be sounded; it is for the log and the scenario checks).
void bluewake_puppet_remote_voice(int id, const char* voice_id, bool local_bank);
// Whether the owner's expression (the texture numbers sent with his pose) is for the model his puppet shows: false when his own
// model is a mod's that is not what the puppet wears (not shared, or not arrived yet). Default true.
void bluewake_puppet_remote_face_ok(int id, bool ok);
// The owner's Link Voice Pitch (a ratio, 1.0 normal): the puppet's voice is made at it.
void bluewake_puppet_remote_voice_pitch(int id, float ratio);
// A puppet's voice from its own player's voice bank: called (on the game thread) when a puppet whose owner's voice bank is not the
// local one starts a voice, with Link's voice id (0x1800 + id), the gains for the left and right channel from the puppet's
// distance and side (1 at most) and the owner's pitch ratio. Returns true when it played the voice (from a decoded bank), and
// the local bank's voice is then not made. False: the local bank plays it, as before.
typedef bool (*BluewakePuppetVoiceFn)(int player_id, int voice_id, float left, float right, float pitch);
void bluewake_puppet_set_voice_player(BluewakePuppetVoiceFn fn);
// J3DMatPacket::draw hook: tells the renderer whose puppet the next material draw belongs to (texture variants).
bool bluewake_puppet_mat_observes(u32 address);
void bluewake_puppet_mat_enter(CPUState* cpu, u32 address);
// Whether that player's puppet may be drawn now (same stage and room).
void bluewake_puppet_remote_show(int id, bool show);
// Whether that player is in my stage and room (an arrow on my minimap, in their color, drawn by the game's own cursor
// object; independent of whether the puppet itself is drawn).
void bluewake_puppet_remote_map(int id, bool on, uint8_t r, uint8_t g, uint8_t b);
// Whether that player is shown on my Sea Chart (any stage), at this world x, z, facing angle (the game's s16 shape angle),
// in their color: drawn in the chart's own J2D tree by the pause menu's 2D draw (puppet.c, the chart pass).
void bluewake_puppet_remote_chart(int id, bool on, float x, float z, int angle, uint8_t r, uint8_t g, uint8_t b);
void bluewake_puppet_remote_remove(int id);
void bluewake_puppet_remote_clear(void);
// Whether the puppet was drawn in the last frame, and where its head is.
bool bluewake_puppet_drawn(int id, float head[3]);
// Where its name tag goes: the head's place on screen (NDC x, y; ndc[2] the clip w) between its last two game frames, each
// against its own camera: weight 0 the older frame, 1 the newer. False when it is behind the camera or not known yet.
bool bluewake_puppet_tag(int id, float weight, float ndc[3]);
// Whether puppets make Link's voice and sound effects as they start actions (rolls, jumps, cuts, damage).
void bluewake_puppet_set_sounds(bool on);
// Any thread: starts a game system sound (JAZelAudio_SE.h: 0x0830 JA_SE_MSEL_CURSOR, ...) at the game's next frame in play, at the game's
// Sound Effects volume, as mDoAud_seStart(id, 0, 0, 0). Up to 8 wait; one that has waited 2 s, or arrives while the game's audio is not
// up (title, file select, loading), is dropped. bluewake_puppet_se_started counts the ones started (for the log and tests).
void bluewake_puppet_play_se(unsigned id);
unsigned bluewake_puppet_se_started(void);
// Where the puppets' fast pool lies in a MEM1 array of this size (32 MB: the top of the dynamic REL window; 64 MB: everything
// above 0x82000000); false when none.
bool bluewake_puppet_fast_range(u32 ram_size, u32* base, u32* bytes);
// A solid heap from the puppets' pools (guest memory outside the game's own heaps), made the current heap (the caller
// restores it); 0 when there is no pool (puppets off, no room): the caller uses the game's heap. The Bunny Hood's models
// use them so that a room of custom-model players does not leave the hood without room (and the game's heaps are the game's).
u32 bluewake_puppet_pool_heap(GuestCoro* co, u32 size);
// Destroys a heap made by it, or any other solid heap, with the pool's bookkeeping.
void bluewake_puppet_pool_destroy(GuestCoro* co, u32 heap);
// (app) An explosion of another player's bomb (one this PC made a harmless copy of) went off near me: whose, and where. True once per
// blast. The app decides whether it hurts: only where the room's PvP rules say that player may hit me (anchor.cpp).
bool bluewake_puppet_take_blast(int* player, float pos[3]);
// Test: the local pose is also fed back as player 1000, offset sideways by this
// many units (BLUEWAKE_PUPPET_SELF).
void bluewake_puppet_self_test(float offset);

#ifdef __cplusplus
}
#endif

#endif
