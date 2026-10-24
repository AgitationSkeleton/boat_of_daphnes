/* Force-included into Boat of Daphnes' sources (src/bod) built on this runtime: what its own Aurora
 * fork and RecompCore give it that ours lack. Declared here, defined by the runtime's adaptation
 * layer (src/runtime/bod_host.cpp). Its sources themselves are not changed. */
#ifndef BOD_COMPAT_H
#define BOD_COMPAT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Boat of Daphnes' Aurora (include/aurora/aurora.h of its fork): render settings. */
void aurora_set_frame_buffer_scale(float scale);
void aurora_set_frame_buffer_aspect(float aspect);
void aurora_set_integer_scale(int mode);
void aurora_set_present_suppressed(bool suppressed);
void aurora_set_forced_anisotropy(unsigned samples);
void aurora_set_frame_interpolation(bool enabled);
void aurora_set_fps_overlay(bool enabled);
bool aurora_get_frame_interpolation(void);
/* The weight of the frame being drawn between the game's older frame (0) and its newer one (1): 1 for a real frame. */
float aurora_get_frame_blend(void);
void aurora_set_frame_interp_steps(int steps);
int aurora_get_frame_interp_steps(void);
unsigned long long aurora_get_shown_frames(void);
typedef struct AuroraFrameInterpTotals {
  unsigned long long frames, interpolated, draws, rejected, unmatched;
} AuroraFrameInterpTotals;
void aurora_get_frame_interp_totals(AuroraFrameInterpTotals* out);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
#include <dolphin/gx.h> /* aurora/texture.hpp names GXTexObj without including it */
#include <imgui.h>
/* Boat of Daphnes' Aurora: a texture whose pixels are replaced as often as needed (the GBA screen). */
extern "C" ImTextureID aurora_imgui_create_dynamic_texture(uint32_t width, uint32_t height);
extern "C" void aurora_imgui_update_texture(ImTextureID id, const void* rgba8);
#endif
#endif
