// The game module: tww_game.dll (tww_game.so on Linux), the game's code translated by the
// recompiler from the player's own disc - built by the launcher on first run (it is never shipped) or, for development, by the
// CMake target sa2b_game. Loaded first thing; its tables and the state its code owns are reached
// through g_game and the pointers below (src/runtime/ctx.h, modules.h).
#include "runtime/modswitch.h"
#include "runtime/modules.h"
#include "runtime/runtime.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <SDL3/SDL_messagebox.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <string>

namespace rt {

const GameModule* g_game = nullptr;

namespace {
// stand-ins until the module is loaded (a flag set from the command line is carried over)
volatile u32 s_irqPending = 0;
bool s_traceOn = false;
u32 s_relBase[kMaxModules][32];
}  // namespace
volatile u32* g_irqPendingPtr = &s_irqPending;
bool* g_traceOnPtr = &s_traceOn;
u32 (*g_relBasePtr)[32] = s_relBase;

// The module next to the program (`rando`: the randomizer's, tww_game_rando); false (after telling the player, unless `quiet`) when it is
// missing or was built for another version of the runtime.
bool loadGameModule(bool quiet, bool rando) {
    std::string error;
    auto fail = [&] {
        std::fprintf(stderr, "%s\n", error.c_str());
#if defined(_WIN32)
        if (!quiet) MessageBoxA(nullptr, error.c_str(), "Boat of Daphnes", MB_OK | MB_ICONINFORMATION);
#else
        if (!quiet) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Boat of Daphnes", error.c_str(), nullptr);
#endif
        return false;
    };
    using Fn = const GameModule* (*)();
    Fn get = nullptr;
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + (rando ? L"tww_game_rando.dll" : L"tww_game.dll");
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) {
        error = "The game has not been set up on this computer yet. Start TWWLauncher.exe: on its first run it builds the game "
                "from your disc.";
        return fail();
    }
    get = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "sa2b_game_module")));
#else
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string path(exe, n > 0 ? size_t(n) : 0);
    path = path.substr(0, path.find_last_of('/') + 1) + (rando ? "tww_game_rando.so" : "tww_game.so");
    // (local: its own names - memcpy and the rest - stay its own; the runtime's are bound from the
    // program's exported symbols)
    if (access(path.c_str(), F_OK) != 0) {
        error = "The game has not been set up on this computer yet. Start Boat of Daphnes: on its first run it builds the game "
                "from your disc.";
        return fail();
    }
    void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {  // (a function it needs that this runtime lacks, say)
        std::fprintf(stderr, "%s\n", dlerror());
        error = "The game was set up by another version of this program. Start Boat of Daphnes to set it up again.";
        return fail();
    }
    get = reinterpret_cast<Fn>(dlsym(m, "sa2b_game_module"));
#endif
    const GameModule* g = get ? get() : nullptr;
    if (!g || g->abi != kGameAbi) {
#if defined(_WIN32)
        error = "The game was set up by another version of this program. Start TWWLauncher.exe to set it up again.";
#else
        error = "The game was set up by another version of this program. Start Boat of Daphnes to set it up again.";
#endif
        return fail();
    }
    *g->irqPending = s_irqPending;
    *g->traceOn = s_traceOn;
    g_irqPendingPtr = g->irqPending;
    g_traceOnPtr = g->traceOn;
    g_relBasePtr = g->relBase;
    g_game = g;
    g_mods[rando ? 1 : 0] = g;
    return true;
}

// The randomizer's module beside the game's (tww_game_rando.dll), for switching to it while the game runs
// (src/runtime/modswitch.cpp): false when it is not there or was built for another runtime.
bool loadSecondModule() {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"tww_game_rando.dll";
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) return false;
    using Fn = const GameModule* (*)();
    const Fn get = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "sa2b_game_module")));
    const GameModule* g = get ? get() : nullptr;
    if (!g || g->abi != kGameAbi) {
        std::fprintf(stderr, "[modswitch] tww_game_rando.dll was built for another version of the runtime\n");
        return false;
    }
    *g->irqPending = 0;
    *g->traceOn = *g_traceOnPtr;
    g_mods[1] = g;
    return true;
#else
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string path(exe, n > 0 ? size_t(n) : 0);
    path = path.substr(0, path.find_last_of('/') + 1) + "tww_game_rando.so";
    if (access(path.c_str(), F_OK) != 0) return false;
    void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        std::fprintf(stderr, "[modswitch] %s\n", dlerror());
        return false;
    }
    using Fn = const GameModule* (*)();
    const Fn get = reinterpret_cast<Fn>(dlsym(m, "sa2b_game_module"));
    const GameModule* g = get ? get() : nullptr;
    if (!g || g->abi != kGameAbi) {
        std::fprintf(stderr, "[modswitch] tww_game_rando.so was built for another version of the runtime\n");
        return false;
    }
    *g->irqPending = 0;
    *g->traceOn = *g_traceOnPtr;
    g_mods[1] = g;
    return true;
#endif
}

// Once guest memory exists: the module's copy of its base.
void gameModuleMemory() {
    for (const GameModule* g : g_mods)
        if (g) *g->mem = g_mem;
}

}  // namespace rt
