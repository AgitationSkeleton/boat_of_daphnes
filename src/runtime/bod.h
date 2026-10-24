#pragma once
// Boat of Daphnes on this runtime. Its menus, windows and settings are its own code, unchanged
// (src/bod: windows/src and runtime/host/src of Boat of Daphnes, AgitationSkeleton, GPL-3.0-or-later);
// src/runtime/bod_host.cpp provides the host API they call (its RecompCore / Aurora fork's), and
// src/runtime/bod_gameopts.cpp its game options through this runtime's interpreter.
#include "runtime/ctx.h"

#include <string>

namespace rt::bod {

// ---- settings: Boat of Daphnes' CVars (boat-of-daphnes.json beside the program) ----
int cvarInt(const char* name, int def);
float cvarFloat(const char* name, float def);
std::string cvarString(const char* name, const char* def);
void cvarSetInt(const char* name, int v);
void cvarSetFloat(const char* name, float v);
void cvarSetString(const char* name, const std::string& v);
void cvarSave();  // now (before the game's frames run: the first-run setup's disc)

// ---- game options (bod_gameopts.cpp): Better Wind Waker's and Boat of Daphnes' switches ----
// The game thread: applies what the settings ask for (once a frame), and a REL's sites as it links.
void optionsFrame();
void optionsLinked(u32 module);
void optionsUnlinked(u32 module);
std::string optionCvar(const char* name);  // gEnhancements.BetterWW.<name>
bool optionSupported(const char* name);
bool optionSet(const char* name, bool on);  // (bluewake_game_options_set)
bool optionHas(const char* name);  // the option list has it (bluewake_game_options_has)
bool optionOn(const char* name);
void introSkipSet(bool on);  // (bluewake_intro_skip_set)
void optionForce(const char* name, float heroMultiplier);  // a randomizer seed's option on (Hero Mode: its multiplier)
void optionForceClear();                                   // ... and off again

// ---- the randomizer (bod_rando.cpp): Boat of Daphnes' seed runtime on the randomizer's game module ----
// Before anything else (main): its settings say whether this session is a randomizer one (the file select restarted
// the game into it), which picks the game module and the disc.
bool randoSessionWanted();
void randoSessionStarts(bool rando, bool canSwitch);  // the session's mode; whether it can switch while the game runs
bool randoSession();
void randoRetrace();                  // the game thread, each retrace: the seed applied once it is chosen
void randoLinked(u32 module);         // a REL's seed sites, data bytes and option words
void randoUnlinked(u32 module);
u32 randoUiHook(Ctx& c);              // its hook 50 (dFile_select_c::_move): the file select's queued sounds and music
void gbaRetrace();                    // the in-app GBA (bod_gba.cpp): its keys and clock, each retrace
void hostShutdown();                  // the process is ending: a restart the file select asked for starts the new one

// ---- the host (bod_host.cpp) ----
void launchBegin();                 // launch recovery's marker: this start has begun (bod_host.cpp)
void hostSettings();                // before the game starts: Boat of Daphnes' settings loaded
void hostInstall();                 // the render thread, once Aurora is up: its launch settings, its menu
void hostOverlay();                 // the render thread, each frame in Aurora's frame: its menu and windows
void hostEvent(const void* sdlEvent);  // the render thread: an SDL event (keys, gamepad, a dropped file)
bool menuOpen();                    // its menu is open: the game's pad input is held
bool interpolationWanted();         // its Interpolation FPS setting asks for in-between frames
void hostRetrace();                 // the game thread, each retrace: its host's retrace work
void hostLinked(u32 module);        // ... a REL linked / unlinked
void hostUnlinked(u32 module);
unsigned treasureGlowHook(Ctx& c, u32 pc);  // its hook 12 (Treasure Light Up Close)
unsigned hostHook(Ctx& c, unsigned id, u32 pc);   // its host's hooks (30/31 draw and delete, 34, 37/38, 51, 70-73)
u32 playerModelHook(Ctx& c);                // its hook 13 (player models without a restart)
void voiceSwapFrame(Ctx& c);                // bod_voice.cpp: a voice change asked for, made at mDoAud_Execute each frame

}  // namespace rt::bod
