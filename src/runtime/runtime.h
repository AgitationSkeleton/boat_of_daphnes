#pragma once
// Runtime internals shared by the host side (memory, dispatch, HLE registry, logging).
#include "runtime/ctx.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace rt {

// ---- logging ----
void log(const char* fmt, ...);
void vlog(const char* fmt, va_list ap);

// ---- memory ----
bool memInit();
bool loadDol(const std::vector<u8>& dol, u32& entry);
inline u32 phys(u32 a) { return a & 0x01FFFFFF; }
// Bump allocator for host-owned guest objects (interrupt stack, HLE structures). They live past
// the end of MEM1, in the rest of the 32 MB guest window: no game code addresses that range, so
// all of MEM1 stays laid out as the IPL leaves it (the game hardcodes buffers up near its top).
constexpr u32 kHleRegion = 0x80000000 + kMem1Size;
// The top 4 MB are the puppets' fast pool (puppet.c, bluewake_puppet_fast_range), so the host's own objects stop below it.
constexpr u32 kHleRegionEnd = 0x80000000 + kHostMemSize - 0x00400000;
u32 hleAlloc(u32 size, u32 align = 32);
// Widescreen changes to the game's code (src/runtime/tww.cpp), while their module is linked.
void gameFixesLinked(u32 module);
void gameFixesUnlinked(u32 module);
void gameFixesInit();   // main.dol changes (src/runtime/tww.cpp), at startup
void gameFixesFrame();  // ... and their per-frame part, on the game thread
extern std::atomic<bool> g_wideCulling, g_wideHud;  // Widescreen Actor Culling and HUD at the Screen Edges (bod_gameopts)
extern std::atomic<float> g_wideAspect;  // the window's aspect when widescreen is on, else 4:3 (frontend)
float twwWideScale();                      // the 2D picture's widening about the middle (1 at 4:3), as the widescreen code has it
void twwWideFrame();                       // the widescreen code, per frame (src/runtime/twwwide.cpp)
extern std::atomic<float> g_wideExtra;  // logical units shown beyond each side of the 4:3 screen (frontend)
void profileRegisterThread();  // src/runtime/profile.cpp: every thread running game code (priority; SA2_PROFILE)
inline const char* gstr(u32 a) { return reinterpret_cast<const char*>(hostp(a)); }

// ---- dispatch ----
void dispatchRegister(u32 addr, GuestFn fn);
void dispatchUnregister(u32 addr);
GuestFn dispatchLookup(u32 addr);
void registerDol();
// Registers / unregisters a REL module's functions once its section bases are known.
void registerRelModule(u32 id);
void unregisterRelModule(u32 id);

// ---- HLE / hooks ----
struct HleReg {
    HleReg(const char* name, HleFn fn);
};
#define HLE(name)                                                   \
    static void hle_##name(::rt::Ctx& c);                            \
    static ::rt::HleReg hleReg_##name(#name, &hle_##name);           \
    static void hle_##name([[maybe_unused]] ::rt::Ctx& c)
void resolveHooks();                         // fills g_hooks from the registry
void callOriginal(Ctx& c, const char* name); // runs the translated body of a hooked function
GuestFn originalOf(const char* name);        // ... that body, to keep (callOriginal looks it up each time)
// A hooked function's body kept at a call site (static OriginalFn s_orig("name"); s_orig(c)): looked up again when the
// module in force changes (modswitch). The other module's body of a hooked function goes back to the hook, so a body
// kept from before a switch called the hook again without end (a stack overflow: user 2026-10-25).
u32 archiveOpens();  // hle_dvd.cpp: archives opened from the disc so far
// For the looks for message files in memory (Instant Text, Crenando, the Sail's name): asked once a second, due for the
// five seconds after an archive opened, and not otherwise. They used to search all 24 MB every second, which cost the
// game a frame about once a second (it ran at 29 of 30: cutscene audio drifted ahead; user 2026-10-25).
struct ArchiveWatch {
    u32 seen = ~0u;
    int left = 0;
    bool due();
};
class OriginalFn {
public:
    explicit OriginalFn(const char* name) : m_name(name) {}
    void operator()(Ctx& c);
private:
    const char* m_name;
    const void* m_module = nullptr;
    GuestFn m_fn = nullptr;
};
u32 guestSymbol(const char* name);           // address of a named main.dol function (0 if unknown)
const char* guestName(u32 addr);             // name of a main.dol function, or nullptr
// Calls a guest function with up to 8 integer arguments on `c`'s stack/registers; returns r3.
u32 guestCall(Ctx& c, u32 fn, std::initializer_list<u32> args = {});
inline u32 stackArg(const Ctx& c, int n) { return rd32(c.r[1] + 8 + 4 * n); }

// ---- formatting of guest printf-style calls (OSReport) ----
std::string guestFormat(Ctx& c, u32 fmtAddr, int firstGpr, int firstFpr);

// ---- device interrupts ----
// The OS keeps its handler table at 0x80003040 (__OSSetInterruptHandler writes entry `id`). Posts
// an interrupt that runs the game's handler for `id`, after `before` sets the device status bits
// the handler checks.
constexpr u32 kInterruptTable = 0x80003040;
void raiseInterrupt(u32 id, void (*before)() = nullptr);

// ---- time ----
u64 timebase();  // guest time base: 40.5 MHz (bus clock / 4) since boot
constexpr u64 kTimebaseHz = 40500000;
// Sleeps until `t` (host steady clock) within about half a millisecond; `spin` lands on it exactly
// (src/runtime/hosttime.cpp).
void sleepUntil(std::chrono::steady_clock::time_point t, bool spin);
// A fast host counter for timing code (SA2_GX_TIMING): in the units profileGuestCycles() counts the
// game threads' CPU time in - time stamp counter cycles on Windows, nanoseconds on Linux.
u64 hostCycles();


}  // namespace rt
