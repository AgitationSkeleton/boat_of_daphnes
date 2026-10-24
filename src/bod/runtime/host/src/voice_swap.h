// Link's voice changed while the game runs (the Mod Menu's voice mods): the game reads JaiInit.aaf and the voice wave
// files once, so the runtime (src/runtime/bod_voice.cpp) overwrites the wave tables of the banks that changed and, when
// their files are in ARAM, the files, on the game thread when no voice is playing from them.
#ifndef BLUEWAKE_VOICE_SWAP_H
#define BLUEWAKE_VOICE_SWAP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The game's audio has registered its wave banks (a swap can be asked for).
bool bluewake_voice_swap_live(void);
// Ask for the swap: old_aaf is the JaiInit.aaf the game loaded, new_aaf the one now wanted, files the wave files of the
// banks that differ ("voice_0.aw", "GanonT_0.aw") by their names. Made at a retrace; false when the request is refused.
bool bluewake_voice_swap(const void* old_aaf, size_t old_n, const void* new_aaf, size_t new_n, const char* const* names,
                         const void* const* data, const size_t* sizes, int count);
// 0 none, 1 asked for and waiting, 2 done, -1 not possible (the voice changes at the next start).
int bluewake_voice_swap_state(void);

#ifdef __cplusplus
}
#endif

#endif
