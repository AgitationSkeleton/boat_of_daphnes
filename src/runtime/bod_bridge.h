#pragma once
// Between the two halves of the adaptation layer: bod_host.cpp (this runtime's side) and
// bod_api.cpp (Boat of Daphnes' API, built with its own headers, which clash with the runtime's).
// Plain types only.
#include <atomic>
#include <cstdint>

namespace rt::bod {
void setOverlay(void (*fn)(void*), void* user);
bool optionSet(const char* name, bool on);
bool optionHas(const char* name);
void introSkipSet(bool on);
extern std::atomic<bool> g_interp, g_fpsOverlay;
extern std::atomic<int> g_interpSteps, g_integerScale;
extern std::atomic<float> g_bufferScale, g_bufferAspect, g_masterVolume, g_gameFps;
extern std::atomic<unsigned> g_anisotropy;
extern std::atomic<unsigned long long> g_shown;
extern std::atomic<bool> g_resetRequested;  // the game asked for a console reset (hle_os OSResetSystem)
// A puppet's own voice mixed into the game's sound (audio.cpp): 32 kHz mono, resampled by step, at the two gains.
void mixVoice(const int16_t* mono, uint32_t frames, float gainLeft, float gainRight, float step);
extern std::atomic<bool> g_fastForward, g_presentSuppressed;  // its Faster Transitions' fast-forward
}  // namespace rt::bod

namespace rt::bod {
// A guest thread's registers, for Boat of Daphnes code that takes its CPUState (copied in and out).
struct Regs {
    unsigned* r;      // 32
    double* f;        // 32
    double* ps1;      // 32
    unsigned* lr;
    unsigned* ctr;
    unsigned char* cr;  // 8 fields, a nibble each (Ctx::cr)
};
unsigned char* guestRam();           // main memory (24 MiB, big-endian), as its CPUState's ram
unsigned long long retraceCount();
// bod_api.cpp: its host code on its CPUState.
void apiAttach();                                       // once, before the game runs
void apiRetrace();                                      // each retrace, on the game thread
void gyroFrame();                                        // each game frame after its work (gyro.cpp)
void apiPads(void* pads, unsigned long long retrace);   // PADStatus[4] as read: the adapter's merged in
bool apiMotorForward(unsigned channel, unsigned motor); // the game's rumble: true to drive the pads
unsigned apiFeatureEnter(Regs regs, unsigned address);  // its intercepts (Forest Water, Quick Doors)
unsigned apiTreasureGlow(Regs regs, unsigned pc);       // hook 12
unsigned apiPaneHook(Regs regs, unsigned pc);           // hook 7 (J2DPane::draw): pane colours
void apiForestWaterLinked(unsigned textStart);          // d_a_obj_ftree (module 317) linked: its section 1
unsigned apiHostHook(Regs regs, unsigned id, unsigned pc);  // its host's hooks, as its game_options.c native_hook sends them
void apiOptionsRetrace();                               // its game_options retrace's host part (bod_gameopts' frame)
void callGuest(unsigned function);                      // bod_host.cpp: a guest call on the hook's own context
bool playerModelLive();                                 // bod_extras.cpp: hook 13 has run
void playerModelRequest();                              // ... a swap of Link's archive at the next play scene
int playerModelState();                                 // ... as bluewake_player_model_state
unsigned apiForestWaterTreeCheck();                     // ... the address its timer check is at (0: none)
bool optionDescribe(unsigned position, const char** name, const char** title, bool* defaultOn, bool* on);
}  // namespace rt::bod
