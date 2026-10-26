#ifndef BLUEWAKE_GAME_OPTIONS_H
#define BLUEWAKE_GAME_OPTIONS_H

#include "core/cpu.h"

#include <stdbool.h>
#include <stdint.h>

// Game options (mods/betterww/options.txt, docs/MODS.md): Better Wind Waker's
// settings as switches. The option sites in the composite read the switches;
// this sets them, runs the native code at the hook sites, writes the options'
// values and patches the message data for instant text.
//
// BLUEWAKE_OPTIONS adjusts the defaults: a comma-separated list of option
// names, each turned on, or off when it starts with '-'; "none" first turns
// every option off. The options take effect only while the mod that carries
// their sites ("betterww") is enabled.

// At boot, after the mods are enabled and before the first dispatch.
void bluewake_game_options_enable(void* lib, CPUState* cpu, bool mod_enabled);
// Once per retrace.
void bluewake_game_options_retrace(CPUState* cpu);
// The options the game module offers, for a settings menu: the name at a
// position (the setting's key), its title, whether it is on by default and
// whether it is on now; NULL past the last, or before the game has started.
const char* bluewake_game_options_describe(u32 position, const char** title, bool* default_on, bool* on);
// Better Wind Waker's "invert camera left and right" is on (the game's C-stick
// then already turns the camera the way the mouse does).
bool bluewake_game_options_invert_camera_x(void);
// The port's own options (index 16 and up in mods/betterww/options.txt): on
// whatever the Better Wind Waker switch says.
bool bluewake_game_options_hud_edges(void);
// Turns an option on or off now (the settings menu). False if there is no such option.
bool bluewake_game_options_set(const char* name, bool on);
// A knob of the Ocarina of Time controls (Enhancements > Ocarina of Time Controls), live. False if there is none.
bool bluewake_game_options_set_value(const char* name, float value);
// The last crouch stab (Enhancements > Gameplay): kind 0 none / 1 stab / 2 power stab (non-zero for about 0.7 s after
// it starts), the swing's serial (counts up per stab, wraps), flags (bit 0: the Master Sword), whether it still plays.
void bluewake_swing_state(uint8_t* kind, uint8_t* serial, uint8_t* flags, bool* active);
// Whether the game module offers an option of that name.
bool bluewake_game_options_has(const char* name);
// Whether an option is on now (false for no such option).
bool bluewake_game_options_get(const char* name);
// Hero Mode's damage multiplier (2.0 by default, Better Wind Waker's; a randomizer seed sets wwrando's 4.0).
void bluewake_game_options_set_hero_multiplier(float multiplier);
// Hook 14: Start skips the intro story told after a new game starts (the menu, live; on by default).
#define BLUEWAKE_INTRO_SKIP_HOOK 14u
void bluewake_intro_skip_set(bool on);

// Player preferences that work in every session, set from the settings menu (live): Controls > Use 'Switch' Targeting Mode,
// Skips & Speed-ups > Remove Title and Ending Videos.
void bluewake_pref_switch_targeting_set(bool on);
void bluewake_pref_remove_videos_set(bool on);

// The game asked for a console reset (OSResetSystem, hook 41): the app starts the game again.
bool bluewake_host_reset_requested(void);

#endif
