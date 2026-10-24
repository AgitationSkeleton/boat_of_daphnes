// The window icon on Linux: assets/icon/boat-of-daphnes.png, embedded at build time (CMake's app_icon.inc). Windows takes
// the icon from the program's resources (boat-of-daphnes.rc) instead. Each including file gets its own copy of the PNG.
#pragma once
#if !defined(_WIN32)

#include <SDL3/SDL.h>

namespace appicon {

#include "app_icon.inc"  // kAppIconPng, kAppIconPngSize

// The icon as RGBA (byte order R, G, B, A), or nullptr; the caller destroys it.
inline SDL_Surface* load() {
    SDL_IOStream* io = SDL_IOFromConstMem(kAppIconPng, kAppIconPngSize);
    SDL_Surface* png = io ? SDL_LoadPNG_IO(io, true) : nullptr;
    if (!png) return nullptr;
    SDL_Surface* rgba = SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(png);
    return rgba;
}

}  // namespace appicon

#endif
