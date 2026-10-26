// DVD: the disc's file system table sits in guest memory the way the IPL leaves it, so the SDK's
// own file-system code (DVDOpen, DVDConvertPathToEntrynum, DVDChangeDir...) runs natively. Only the
// reads are replaced: a worker thread serves them from the user's disc image and completes each one
// through the interrupt path (callbacks run in interrupt context, as on the console).
#include "disc/discimage.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <cstring>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <strings.h>
#define _strnicmp strncasecmp
#endif

namespace rt {
u64 gxFrames();  // src/runtime/gx.cpp

namespace {
std::unique_ptr<disc::Image> s_disc;  // .iso/.gcm, .ciso, .rvz or .wia (src/disc)
std::vector<u8> s_fstCopy;  // the FST as read from the disc, for SA2_DVD_LOG checks
std::mutex s_discLock;

// The two programs' discs as one (switching to the randomizer while the game runs, src/runtime/modswitch.cpp): the
// randomizer's disc is the disc (its file table has every file of the game's and its own three), and the game's disc
// is read at kSecondBase and up. The files the two have differently are pointed at one image or the other.
constexpr u64 kSecondBase = 0x20000000u;
std::unique_ptr<disc::Image> s_disc2;
struct UnionFile {
    u32 entry, w1, w2, w1v, w2v;  // its FST entry; offset and length on the randomizer's disc, then on the game's
};
std::vector<UnionFile> s_union;
bool s_unionRando = false;

// DVDCommandBlock (2001 SDK)
enum : u32 {
    CB_COMMAND = 0x08,
    CB_STATE = 0x0C,
    CB_OFFSET = 0x10,
    CB_LENGTH = 0x14,
    CB_ADDR = 0x18,
    CB_CURR = 0x1C,
    CB_TRANSFERRED = 0x20,
    CB_ID = 0x24,
    CB_CALLBACK = 0x28,
};
enum : u32 { ST_END = 0, ST_BUSY = 1, ST_CANCELED = 10 };

struct Request {
    u32 block, addr, length, callback;
    u64 offset;
    u64 seq;
    u32 prio = 2;  // the SDK's 4 waiting queues: 0 is served first (DVDReadAbsAsyncPrio's last argument)
};
// The latest request issued on each command block: a completion for an older one (the game
// cancelled it and reused the block) must not finish the new request with the old data.
std::mutex s_seqLock;
std::unordered_map<u32, u64> s_blockSeq;
u64 s_seq = 0;
std::atomic<u32> s_inFlight{0};
std::mutex s_qLock;
std::condition_variable s_qCv;
std::deque<Request> s_queue;
bool s_workerStarted = false;

// ---- file overlay (Boat of Daphnes' dvd_overlay_set: mods, player models, randomizer data) ----
// A disc file served from memory: its FST entry (in guest memory, where the game's own file-system
// code reads it) is pointed at an overlay range above every real disc offset, with the new length;
// reads there come from the copy. A file already open keeps what it read. The ranges are packed by
// size (the FST's offsets are 32-bit, and a randomizer seed puts some 200 files in); a range is kept
// for reuse when its file is taken out.
constexpr u64 kOverlayBase = 0x80000000u, kOverlayEnd = 0x100000000ull, kOverlayAlign = 0x8000u;
struct Overlay {
    std::vector<u8> data;
    u32 entry = 0, w1 = 0, w2 = 0;  // the FST entry (0: the range is free) and what it held
    u64 base = 0, capacity = 0;     // its disc offset range
};
std::mutex s_overlayLock;
std::vector<std::unique_ptr<Overlay>> s_overlays;
u64 s_overlayNext = kOverlayBase;

bool readRaw(u64 offset, void* dst, u32 len) {
    if (offset >= kOverlayBase) {
        std::lock_guard g(s_overlayLock);
        for (const auto& o : s_overlays) {
            if (!o->entry || offset < o->base || offset >= o->base + o->capacity) continue;
            const u64 at = offset - o->base;
            std::memset(dst, 0, len);
            if (at < o->data.size()) std::memcpy(dst, o->data.data() + at, std::min<u64>(len, o->data.size() - at));
            return true;
        }
        return false;
    }
    std::lock_guard g(s_discLock);
    if (s_disc2 && offset >= kSecondBase) return s_disc2->read(offset - kSecondBase, dst, len);
    return s_disc && s_disc->read(offset, dst, len);
}

// The FST entry of a path ("/res/Object/Link.arc" or without the leading slash), in an FST at
// `fst` (guest memory, or a copy): 0 if none. Names compare without case, as the game's do.
u32 fstFind(const u8* fst, const char* path) {
    auto w = [&](u32 i, u32 k) { const u8* q = fst + i * 12 + k * 4; return u32(q[0]) << 24 | u32(q[1]) << 16 | u32(q[2]) << 8 | q[3]; };
    const u32 n = w(0, 2);
    const char* strtab = reinterpret_cast<const char*>(fst + n * 12);
    while (*path == '/') path++;
    u32 dirEnd = n, i = 1;
    while (*path) {
        const char* slash = std::strchr(path, '/');
        const size_t len = slash ? size_t(slash - path) : std::strlen(path);
        bool found = false;
        while (i < dirEnd) {
            const bool isDir = (w(i, 0) >> 24) != 0;
            const char* name = strtab + (w(i, 0) & 0xFFFFFF);
            if (std::strlen(name) == len && !_strnicmp(name, path, len)) {
                if (slash && !isDir) return 0;
                found = true;
                break;
            }
            i = isDir ? w(i, 2) : i + 1;  // past a directory's whole contents
        }
        if (!found) return 0;
        if (!slash) return i;
        dirEnd = w(i, 2), i++;
        path = slash + 1;
    }
    return 0;
}

bool readRaw(u64 offset, void* dst, u32 len);

void complete(const Request& r, s32 result) {
    s_inFlight--;
    sched::postIrq([r, result](Ctx& c) {
        {
            std::lock_guard g(s_seqLock);
            if (s_blockSeq[r.block] != r.seq) return;  // stale: cancelled or superseded
        }
        if (rd32(r.block + CB_STATE) == ST_CANCELED) return;
        wr32(r.block + CB_STATE, result >= 0 ? ST_END : u32(-1));
        wr32(r.block + CB_TRANSFERRED, result >= 0 ? u32(result) : 0);
        if (r.callback) {
            c.r[3] = u32(result);
            c.r[4] = r.block;
            c.call(r.callback);
        }
    });
}

// ---- the drive's timing ----
// A GameCube disc spins at a constant 28.5 turns a second; data sits on a spiral from 24 mm out
// to 38 mm at constant density, so a read streams at 2.1 MiB/s at the hub up to 3.3 MiB/s at the
// rim. The drive reads whole 32 KiB ECC blocks; moving the head costs a seek (35 ms plus head
// travel within a millimetre, 75 ms plus travel beyond) and then the wait for the block to come
// round. While idle it keeps reading ahead up to 1 MiB past the last block it was asked for, and
// what it already holds goes to memory at 32 MiB/s. (The figures are the hardware measurements
// Dolphin's drive model uses, the reference this runtime is checked against.)
const auto s_clock0 = std::chrono::steady_clock::now();
double hostSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_clock0).count(); }
// The Wind Waker reads fast by default (shorter loads: less time on the black of a scene change);
// SA2_CONSOLE_DISC gives the console drive's timing. (With that timing and requests served in plain
// order, streamed music's reads waited behind a whole stage load: JAudio's audio thread overflowed
// its message queue - "syncDSP : Send Miss" - and audio stalled for good. Requests are served by
// priority now, as the SDK does: see the worker.)
bool fastDisc() {  // (asked once the command line has been read)
    static const bool fast = std::getenv("SA2_FAST_DISC") != nullptr || std::getenv("SA2_CONSOLE_DISC") == nullptr;
    return fast;
}

struct Drive {
    static constexpr u64 kBlock = 32 * 1024, kAhead = 1024 * 1024;
    static constexpr double kInner = 0.024, kOuter = 0.038;
    double busyUntil = 0;
    // Read-ahead: blocks [aheadFrom, aheadTo) arrive between aheadT0 and aheadT1 (none if equal).
    u64 aheadFrom = 0, aheadTo = 0;
    double aheadT0 = 0, aheadT1 = 0;

    static double radius(u64 off) {  // where an offset lies: area grows with r^2 (single-layer DVD density)
        constexpr double layer = 0x118240000, rimOfDvd = 0.058;
        return std::sqrt(double(off) / layer * (rimOfDvd * rimOfDvd - kInner * kInner) + kInner * kInner);
    }
    static double streamTime(u64 off, u64 len) {
        const double r = std::min(radius(off + len / 2), kOuter);
        const double speed = (2.1 + (r - kInner) / (kOuter - kInner) * (3.325 - 2.1)) * 1024 * 1024;
        return double(len) / speed;
    }
    static double seekTime(u64 from, u64 to) {
        const double d = std::fabs(radius(from) - radius(to));
        return d < 0.001 ? 0.035 + d * 50 : 0.075 + d * 4.5;
    }
    static double turnWait(u64 off, double t) {  // until the block's angle comes under the head
        const double target = std::fmod(radius(off) / 0.00000074, 1.0), now = std::fmod(t * 28.5, 1.0);
        return std::fmod(target + 1.0 - now, 1.0) / 28.5;
    }

    // When a read of [off, off+len) issued at time t completes.
    double read(u64 off, u64 len, double t) {
        // What the read-ahead holds by now, and where the head is.
        u64 heldFrom = 0, heldTo = 0, head = 0;
        if (aheadT1 > aheadT0) {
            heldFrom = aheadTo > kAhead ? aheadTo - kAhead : 0;
            heldTo = t >= aheadT1 ? aheadTo
                                  : aheadFrom + u64((t - aheadT0) / (aheadT1 - aheadT0) * double(aheadTo - aheadFrom)) / kBlock * kBlock;
            head = heldTo;
        }
        u64 block = off / kBlock * kBlock;
        if (block < heldFrom) heldFrom = heldTo = 0;  // reading behind the buffer drops it
        const u64 first = block;
        double done = t + 0.0006;  // the command itself
        bool sought = false;
        for (u64 pos = off, left = len; left;) {
            const u64 chunk = std::min<u64>(left, (pos / kBlock + 1) * kBlock - pos);
            if (block >= heldFrom && block < heldTo) {
                done += double(chunk) / (32.0 * 1024 * 1024);
            } else {
                if (block != head) {
                    sought = true;
                    done += seekTime(head, block);
                    done += turnWait(block, done);
                } else {
                    done += streamTime(block, kBlock);
                }
                head = block + kBlock;
            }
            pos += chunk, left -= chunk, block += kBlock;
        }
        // The drive goes on reading ahead from the last block (keeping it and the one before).
        const u64 last = block - kBlock;
        if (!(last - heldFrom <= 2 * kBlock && heldFrom != heldTo)) {
            aheadFrom = last >= heldTo ? last : heldTo;
            aheadTo = last + kAhead - 2 * kBlock;
            if (sought) aheadTo = std::max(aheadTo, first + kAhead);
            aheadT0 = done;
            aheadT1 = done + streamTime(aheadFrom, aheadTo - aheadFrom);
        }
        return done;
    }
} s_drive;

void worker() {
    profileRegisterThread();
    log("[dvd] drive timing: %s\n", fastDisc() ? "fast (--fast-disc)" : "as the console's drive");
    std::vector<u8> buf;
    for (;;) {
        Request r;
        {
            std::unique_lock lk(s_qLock);
            s_qCv.wait(lk, [] { return !s_queue.empty(); });
            // As the SDK's DVD queue: the highest priority first, in order within one (The Wind
            // Waker's streamed music reads at a high priority while a stage loads at a lower one;
            // served in plain order, the music's next block waited behind the whole stage).
            auto it = std::min_element(s_queue.begin(), s_queue.end(), [](const Request& a, const Request& b) { return a.prio < b.prio; });
            r = *it;
            s_queue.erase(it);
        }
        buf.resize(r.length);
        const bool ok = readRaw(r.offset, buf.data(), r.length);
        // A read takes as long as on the console's drive: the game's streams (music, the movie
        // layers of cutscenes) start when their data arrives, and cutscenes are timed for that.
        // --fast-disc (SA2_FAST_DISC): nearly instant instead.
        const double start = std::max(hostSeconds(), s_drive.busyUntil);
        const double done = fastDisc() ? start + 0.0005 + r.length / 16e6 : s_drive.read(r.offset, r.length, start);
        s_drive.busyUntil = done;
        sleepUntil(s_clock0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(done)), false);
        if (ok) std::memcpy(hostp(r.addr), buf.data(), r.length);
        complete(r, ok ? s32(r.length) : -1);
    }
}

// File containing a disc offset (from the FST in guest memory), for SA2_DVD_LOG.
std::string fileAt(u64 offset) {
    const u32 fst = rd32(0x80000038);
    if (!fst) return "?";
    const u32 n = rd32(fst + 8);
    const u32 strtab = fst + n * 12;
    // Walk the tree keeping the directory path of each entry.
    std::vector<std::pair<u32, std::string>> dirs{{n, ""}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        const u32 w0 = rd32(fst + i * 12), w1 = rd32(fst + i * 12 + 4), w2 = rd32(fst + i * 12 + 8);
        const std::string name = gstr(strtab + (w0 & 0xFFFFFF));
        if (w0 >> 24) dirs.push_back({w2, dirs.back().second + name + "/"});
        else if (offset >= w1 && offset < u64(w1) + w2) return dirs.back().second + name + "+" + std::to_string(offset - w1);
    }
    return "?";
}

void submit(u32 block, u32 addr, u32 length, u64 offset, u32 callback, u32 command, u32 prio = 2) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    if (s_log) log("[dvd] frame %llu: read %s (%u bytes) -> %08X\n", (unsigned long long)gxFrames(), fileAt(offset).c_str(), length, addr);
    wr32(block + CB_COMMAND, command);
    wr32(block + CB_STATE, ST_BUSY);
    wr32(block + CB_ADDR, addr);
    wr32(block + CB_LENGTH, length);
    wr32(block + CB_OFFSET, u32(offset));
    wr32(block + CB_TRANSFERRED, 0);
    wr32(block + CB_CALLBACK, callback);
    u64 seq;
    {
        std::lock_guard g(s_seqLock);
        seq = s_blockSeq[block] = ++s_seq;
    }
    s_inFlight++;
    std::lock_guard g(s_qLock);
    if (!s_workerStarted) {
        s_workerStarted = true;
        std::thread(worker).detach();
    }
    s_queue.push_back({block, addr, length, callback, offset, seq, std::min(prio, 3u)});
    s_qCv.notify_one();
}
}  // namespace

// For stall reports: requests issued but not yet completed, and queued ones.
std::string dvdPendingSummary() {
    std::lock_guard g(s_qLock);
    std::string out = "in flight " + std::to_string(s_inFlight.load()) + ", queued " + std::to_string(s_queue.size());
    for (const Request& r : s_queue) out += "; " + fileAt(r.offset);
    return out;
}

void dvdOpenDisc(const std::string& path) {
    std::string err;
    s_disc = disc::open(std::filesystem::path(path), err);
    if (!s_disc) fatal("can't open disc image %s: %s", path.c_str(), err.c_str());
    // The apploader loads the FST to the top of MEM1 (room for the header's maximum FST size),
    // records it in the boot block and puts arena hi just below it. The game relies on this: it
    // hardcodes buffers (movie work area, module load address) up to a few hundred KB below.
    u8 h[12];
    readRaw(0x424, h, 12);
    auto be = [&](int o) { return u32(h[o]) << 24 | h[o + 1] << 16 | h[o + 2] << 8 | h[o + 3]; };
    const u32 fstOff = be(0), fstSize = be(4), fstMax = std::max(be(8), fstSize);
    const u32 fst = (0x80000000 + kMem1Size - fstMax) & ~31u;
    readRaw(fstOff, hostp(fst), fstSize);
    s_fstCopy.assign(static_cast<const u8*>(hostp(fst)), static_cast<const u8*>(hostp(fst)) + fstSize);
    wr32(0x80000038, fst);
    wr32(0x8000003C, fstSize);
    wr32(0x80000034, fst);  // arena hi
    log("[dvd] disc %s (%s), FST %u bytes at %08X\n", path.c_str(), s_disc->format(), fstSize, fst);
}

namespace {
// A file's place on the disc as the program in force has it: where an overlay holds the file, what the overlay gives
// back when it is taken out.
void setBase(u32 entry, u32 w1, u32 w2) {
    const u32 fst = rd32(0x80000038);
    std::lock_guard g(s_overlayLock);
    for (auto& o : s_overlays)
        if (o->entry == entry) {
            o->w1 = w1, o->w2 = w2;
            return;
        }
    wr32(fst + entry * 12 + 4, w1);
    wr32(fst + entry * 12 + 8, w2);
}
}  // namespace

void dvdUnionMode(bool rando) {
    if (s_union.empty()) return;
    for (const UnionFile& f : s_union) setBase(f.entry, rando ? f.w1 : f.w1v, rando ? f.w2 : f.w2v);
    s_unionRando = rando;
    log("[dvd] %zu files as the %s disc has them\n", s_union.size(), rando ? "randomizer's" : "game's");
}
bool dvdUnionActive() { return !s_union.empty(); }

// The randomizer's disc as the disc, the game's beside it (`changed`: the paths the two have differently, one a line,
// tools/rando/install_dev.py). The session starts as the game's. False (nothing changed) if they do not fit together.
bool dvdOpenUnion(const std::string& randoIso, const std::string& gameIso, const std::string& changed) {
    std::string err;
    auto second = disc::open(std::filesystem::path(gameIso), err);
    std::ifstream list(changed);
    if (!second || !list) return false;
    auto fstOf = [](disc::Image& d, std::vector<u8>& out) {
        u8 h[12];
        if (!d.read(0x424, h, 12)) return false;
        auto be = [&](int o) { return u32(h[o]) << 24 | h[o + 1] << 16 | h[o + 2] << 8 | h[o + 3]; };
        out.resize(be(4));
        return d.read(be(0), out.data(), u32(out.size()));
    };
    std::vector<u8> gameFst;
    if (!fstOf(*second, gameFst)) return false;
    dvdOpenDisc(randoIso);
    const u32 fst = rd32(0x80000038);
    const u8* f = static_cast<const u8*>(hostp(fst));
    auto w = [](const u8* t, u32 i, u32 k) { const u8* q = t + i * 12 + k * 4; return u32(q[0]) << 24 | u32(q[1]) << 16 | u32(q[2]) << 8 | q[3]; };
    // Every file of the randomizer's disc must sit below where the game's disc is read.
    for (u32 i = 1, n = w(f, 0, 2); i < n; i++)
        if (!(w(f, i, 0) >> 24) && u64(w(f, i, 1)) + w(f, i, 2) > kSecondBase) {
            log("[dvd] %s is too big to stand beside the game's disc\n", randoIso.c_str());
            return false;
        }
    std::string path;
    while (std::getline(list, path)) {
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (path.empty()) continue;
        const u32 e = fstFind(f, path.c_str()), ev = fstFind(gameFst.data(), path.c_str());
        if (!e || !ev) continue;
        s_union.push_back({e, w(f, e, 1), w(f, e, 2), u32(kSecondBase + w(gameFst.data(), ev, 1)), w(gameFst.data(), ev, 2)});
    }
    {
        std::lock_guard g(s_discLock);
        s_disc2 = std::move(second);
    }
    dvdUnionMode(false);
    // (the copy disc readers use - the menu's art, the stock title logo - is the game's disc as the session starts)
    s_fstCopy.assign(f, f + rd32(0x8000003C));
    log("[dvd] the game's disc %s beside it: %zu files differ\n", gameIso.c_str(), s_union.size());
    return true;
}

// Path lookups and directory changes stay native; with SA2_DVD_LOG they are logged.
// Archives opened from the disc (message files come in them): the scans for message files look only after one.
std::atomic<u32> g_archiveOpens{0};
u32 archiveOpens() { return g_archiveOpens.load(std::memory_order_relaxed); }
bool ArchiveWatch::due() {
    if (const u32 n = archiveOpens(); n != seen) seen = n, left = 5;  // (a file arrives a little after its archive opens)
    if (left <= 0) return false;
    left--;
    return true;
}
HLE(DVDConvertPathToEntrynum) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    // Testing: SA2_STAGE=<level>[:<character>] - whichever stage the game loads (the attract demo's
    // City Escape), stage <level> is loaded instead, with the level number (r13-0x7AA0) and both
    // players' characters (r13-0x7A98, -0x7A94) set to match as the stage module is opened.
    // Levels 60-67 are boss modules, 70/71 the kart races (the stage loader's own name table).
    if (const char* st = std::getenv("SA2_STAGE")) {
        const char* p = gstr(c.r[3]);
        int n;
        if (std::sscanf(p, "stg%2dD.rel", &n) == 1 && std::strlen(p) == 10) {
            static const char* const kBoss[] = {"boss_bigfoot", "boss_hotshot", "boss_fdog", "boss_bigbogy",
                                                "boss_golem",   "boss_last1",   "boss_last2", "boss_golemE"};
            const int level = std::atoi(st);
            const char* colon = std::strchr(st, ':');
            const u32 chr = colon ? u32(std::atoi(colon + 1)) : 0;
            char name[32];
            if (level >= 60 && level <= 67) std::snprintf(name, sizeof name, "%sD.rel", kBoss[level - 60]);
            else if (level == 70 || level == 71) std::snprintf(name, sizeof name, "CartD.rel");
            else std::snprintf(name, sizeof name, "stg%02dD.rel", level);
            static const u32 s_name = hleAlloc(32);
            std::memcpy(hostp(s_name), name, std::strlen(name) + 1);
            c.r[3] = s_name;
            wr16(c.r[13] - 0x7AA0, u32(level));
            wr32(c.r[13] - 0x7A98, chr);
            wr32(c.r[13] - 0x7A94, chr);
            log("[test] level %d (%s) with character %u instead of stage %d\n", level, name, chr, n);
        }
    }
    const std::string path = gstr(c.r[3]);
    callOriginal(c, "DVDConvertPathToEntrynum");
    if (path.size() > 4 && (path.compare(path.size() - 4, 4, ".arc") == 0 || path.compare(path.size() - 4, 4, ".ARC") == 0))
        g_archiveOpens.fetch_add(1, std::memory_order_relaxed);
    if (s_log) log("[dvd] path '%s' -> entry %d\n", path.c_str(), s32(c.r[3]));
    if (s_log && s32(c.r[3]) < 0) {
        // Is the FST in guest memory still the one read from the disc?
        const u32 fst = rd32(0x80000038);
        const u8* g = static_cast<const u8*>(hostp(fst));
        u32 diffs = 0, first = 0;
        for (u32 i = 0; i < s_fstCopy.size(); i++)
            if (g[i] != s_fstCopy[i] && diffs++ == 0) first = i;
        log("[dvd] FST at %08X: %u bytes differ from the disc's", fst, diffs);
        if (diffs) log(" (first at +%X = %08X)", first, fst + first);
        log("; current dir %d\n", s32(rd32(c.r[13] - 0x7568)));
    }
}
HLE(DVDChangeDir) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    const std::string path = gstr(c.r[3]);
    callOriginal(c, "DVDChangeDir");
    if (s_log) log("[dvd] chdir '%s' -> %d\n", path.c_str(), s32(c.r[3]));
}

HLE(__DVDInitWA) {}
HLE(DVDReset) {}
HLE(DVDCheckDisk) { c.r[3] = 1; }
HLE(DVDGetDriveStatus) { c.r[3] = 0; }  // idle
HLE(DVDReadAbsAsyncPrio) {  // (block, addr, length, offset, callback, prio)
    submit(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], 1, c.r[8]);
    c.r[3] = 1;
}
HLE(DVDReadAbsAsyncForBS) {  // (block, addr, length, offset, callback)
    submit(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], 4);
    c.r[3] = 1;
}
HLE(DVDReadDiskID) {  // (block, diskID, callback)
    submit(c.r[3], c.r[4], 0x20, 0, c.r[5], 5);
    c.r[3] = 1;
}
void invalidate(u32 block) {
    std::lock_guard g(s_seqLock);
    s_blockSeq[block] = ++s_seq;
}

HLE(DVDCancelAsync) {  // (block, callback)
    const u32 block = c.r[3], cb = c.r[4];
    invalidate(block);
    wr32(block + CB_STATE, ST_CANCELED);
    if (cb) sched::postIrq([block, cb](Ctx& ic) {
        ic.r[3] = 0;
        ic.r[4] = block;
        ic.call(cb);
    });
    c.r[3] = 1;
}
HLE(DVDCancel) {
    invalidate(c.r[3]);
    wr32(c.r[3] + CB_STATE, ST_CANCELED);
    c.r[3] = 0;
}

}  // namespace rt

// ---- Boat of Daphnes' overlay API (its gxruntime/dvd.h) ----
extern "C" bool dvd_overlay_set(const char* path, const rt::u8* data, rt::u32 size) {
    using namespace rt;
    const u32 fst = rd32(0x80000038);
    if (!fst || !path) return false;
    const u32 e = fstFind(static_cast<const u8*>(hostp(fst)), path);
    if (!e || (rd32(fst + e * 12) >> 24)) return false;  // not a file
    std::lock_guard g(s_overlayLock);
    Overlay* o = nullptr;
    for (auto& k : s_overlays)
        if (k->entry == e) o = k.get();
    if (!o) {  // the smallest free range that holds it
        for (auto& k : s_overlays)
            if (!k->entry && k->capacity >= size && (!o || k->capacity < o->capacity)) o = k.get();
        if (o) o->entry = e, o->w1 = rd32(fst + e * 12 + 4), o->w2 = rd32(fst + e * 12 + 8);
    }
    if (!o || o->capacity < size) {  // a new range
        const u64 capacity = std::max<u64>(kOverlayAlign, (u64(size) + kOverlayAlign - 1) / kOverlayAlign * kOverlayAlign);
        if (s_overlayNext + capacity > kOverlayEnd) {
            log("[dvd] overlay %s: no disc offset range left\n", path);
            return false;
        }
        if (!o) {
            s_overlays.push_back(std::make_unique<Overlay>());
            o = s_overlays.back().get();
            o->entry = e, o->w1 = rd32(fst + e * 12 + 4), o->w2 = rd32(fst + e * 12 + 8);
        }
        o->base = s_overlayNext, o->capacity = capacity;
        s_overlayNext += capacity;
    }
    o->data.assign(data, data + size);
    wr32(fst + e * 12 + 4, u32(o->base));
    wr32(fst + e * 12 + 8, size);
    log("[dvd] overlay %s: %u bytes (was %u)\n", path, size, o->w2);
    return true;
}
// Whether the file is served from an overlay now (an in-place randomizer switch reopens the disc and drops them).
extern "C" bool dvd_overlay_has(const char* path) {
    using namespace rt;
    const u32 fst = rd32(0x80000038);
    if (!fst || !path) return false;
    const u32 e = fstFind(static_cast<const u8*>(hostp(fst)), path);
    std::lock_guard g(s_overlayLock);
    for (auto& o : s_overlays)
        if (e && o->entry == e) return true;
    return false;
}
extern "C" bool dvd_overlay_clear(const char* path) {
    using namespace rt;
    const u32 fst = rd32(0x80000038);
    if (!fst || !path) return false;
    const u32 e = fstFind(static_cast<const u8*>(hostp(fst)), path);
    std::lock_guard g(s_overlayLock);
    for (auto& o : s_overlays)
        if (e && o->entry == e) {
            wr32(fst + e * 12 + 4, o->w1);
            wr32(fst + e * 12 + 8, o->w2);
            o->entry = 0;  // the range stays, for another file
            o->data = {};
            return true;
        }
    return false;
}
extern "C" void dvd_overlay_clear_all(void) {
    using namespace rt;
    const u32 fst = rd32(0x80000038);
    std::lock_guard g(s_overlayLock);
    for (auto& o : s_overlays) {
        if (o->entry && fst) wr32(fst + o->entry * 12 + 4, o->w1), wr32(fst + o->entry * 12 + 8, o->w2);
        o->entry = 0;
        o->data = {};
    }
}
extern "C" rt::u8* dvd_read_image_file(const char* path, rt::u32* size) {
    using namespace rt;
    if (s_fstCopy.empty() || !path) return nullptr;
    const u32 e = fstFind(s_fstCopy.data(), path);
    if (!e) return nullptr;
    auto w = [&](u32 k) { const u8* q = s_fstCopy.data() + e * 12 + k * 4; return u32(q[0]) << 24 | u32(q[1]) << 16 | u32(q[2]) << 8 | q[3]; };
    if (w(0) >> 24) return nullptr;
    u8* out = static_cast<u8*>(std::malloc(w(2) ? w(2) : 1));
    if (!out || !readRaw(w(1), out, w(2))) {
        std::free(out);
        return nullptr;
    }
    if (size) *size = w(2);
    return out;
}

extern "C" unsigned bluewake_archive_opens(void) { return rt::archiveOpens(); }
