// Link's voice changed while the game runs (the Mod Menu's voice mods; BR-0037). The game reads JaiInit.aaf and the wave
// files once: JAudio's init copies every wave system's table into wave banks on its heap (JASWSParser: a TSimpleWaveBank
// per file, one TWaveInfo per wave) and a bank's .aw file is read into ARAM when its wave group is loaded (voice_0.aw at
// the start, GanonT_0.aw with Ganondorf's tower). A voice mod changes the tables of those two banks and their files and
// nothing else in the init file, so this swaps exactly that, on the game thread at the start of mDoAud_Execute:
//   - the aaf is compared, record by record, with the one the game loaded: the bank tables, the sound table and the
//     other records must be the same, and only wave systems of files the caller gave may differ (else it fails, and the
//     voice changes at the next start, as before);
//   - each such bank's wave infos are overwritten from the new table (all the fields of JASWSParser::createSimpleWaveBank
//     except the arc pointer the bank keeps), in place, so nothing the game holds on to moves;
//   - a file in ARAM is replaced the way the game replaces a scene's: the bank's ARAM block is freed (TWaveArc::erase; its
//     "loaded" flag goes to 0, and a sound asked for meanwhile finds no wave and is not played), a block of the new size
//     is taken from the same heap (THeap::alloc) and filled from the new file here (no disc read), and the flag and the
//     state say loaded again. It waits while a DSP voice reads from the old block. If the heap has no room for the new
//     size the old block is taken again and refilled, and the swap fails;
//   - a file not loaded yet only has its size updated: the game reads the new one (the overlay) when it loads it.
// Nothing is written until every check has passed. Layouts: tww decompilation's JASSimpleWaveBank.h, JASWaveArcLoader.h,
// JASHeapCtrl.h, JASWSParser.h (GZLE01 addresses from its symbols).
#include "runtime/bod.h"
#include "runtime/bod_bridge.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rt {
u8* aramPtr(u32 a);                                  // hle_hw.cpp
bool zeldaAramInUse(u32 lo, u32 hi);                 // zelda_dsp.cpp: a DSP voice reads from [lo, hi)
}

namespace rt::bod {

namespace {
enum : u32 {
    WAVEBANK_TABLE = 0x803F74D4u,  // JASystem::WaveBankMgr::sWaveBank (TWaveBank**)
    WAVEBANK_COUNT = 0x803F74D0u,  // ... sTableSize
    // TSimpleWaveBank (TWaveBank vptr, then TWaveArc at +4)
    BANK_HEAP = 0x08,        // TWaveArc.mHeap (a THeap)
    BANK_ARAM_BASE = 0x44,   // ... its mBase (+0x3C)
    BANK_LOADED = 0x50,      // field_0x4c: 1 while the waves can be played (the waves' field_0x24 points at it)
    BANK_ARC_STATE = 0x6C,   // field_0x68: 0 not loaded, 1 loading, 2 loaded
    BANK_ENTRY = 0x70,       // mEntryNum
    BANK_SIZE = 0x74,        // mSize: the file's length
    BANK_WAVE_TABLE = 0x7C,  // TWaveHandle[] (stride 0x34: vptr, TWaveInfo 0x2C, heap pointer)
    BANK_WAVE_COUNT = 0x80,
    HANDLE_STRIDE = 0x34,
    HANDLE_INFO = 4,
    INFO_SIZE = 0x2C,
    INFO_ARC_PTR = 0x24,  // not copied: the bank's own
    kWaitFrames = 1800,   // a voice that plays for 30 s or a file still loading: give up
    FN_ARC_ERASE = 0x802880A0u,   // JASystem::TWaveArc::erase(this = the bank + 4)
    FN_HEAP_ALLOC = 0x8027CD38u,  // JASystem::Kernel::THeap::alloc(this, mother, size): bool
    ARAM_ROOT_HEAP = 0x803E9214u, // JASystem::WaveArcLoader::sAramHeap
    kAramSize = 16 << 20,
};

struct Swap {
    std::vector<u8> oldAaf, newAaf;
    struct File {
        std::string name;
        std::vector<u8> bytes;
    };
    std::vector<File> files;
};

std::mutex s_lock;
std::unique_ptr<Swap> s_pending;
std::atomic<int> s_state{0};  // 0 idle/done, 1 asked for, 2 done, -1 failed
std::atomic<bool> s_live{false};
int s_wait = 0;

// Big-endian reads that fail outside the buffer.
struct Be {
    const std::vector<u8>& b;
    bool u32at(size_t at, u32& v) const {
        if (at > b.size() || b.size() - at < 4) return false;
        v = u32(b[at]) << 24 | u32(b[at + 1]) << 16 | u32(b[at + 2]) << 8 | b[at + 3];
        return true;
    }
};

struct Blob {
    u32 type = 0, index = 0;
    size_t offset = 0, size = 0;
    std::string name;  // wave systems: the group's file
    u32 waves = 0;
};

// The records of a JaiInit.aaf (JAInter::InitData::checkInitDataOnMemory's walk) as blobs.
bool parseAaf(const std::vector<u8>& aaf, std::vector<Blob>& out, std::string& why) {
    out.clear();
    const Be r{aaf};
    size_t at = 0;
    for (int record = 0; record < 64; ++record) {
        u32 type;
        if (!r.u32at(at, type)) { why = "the init file ends early"; return false; }
        at += 4;
        if (type == 0) return true;
        if (type == 2 || type == 3) {
            for (u32 i = 0;; ++i) {
                u32 offset, size, id;
                if (!r.u32at(at, offset)) { why = "a table runs off the end"; return false; }
                if (offset == 0) { at += 4; break; }
                if (!r.u32at(at + 4, size) || !r.u32at(at + 8, id) || i > 256) { why = "a table is damaged"; return false; }
                at += 12;
                if (offset > aaf.size() || size > aaf.size() - offset) { why = "a table entry lies outside the file"; return false; }
                Blob b;
                b.type = type, b.index = i, b.offset = offset, b.size = size;
                if (type == 3) {
                    u32 winf, groups, group, count;
                    if (size < 0x20 || std::memcmp(&aaf[offset], "WSYS", 4) != 0 || !r.u32at(offset + 0x10, winf) || winf > size ||
                        size - winf < 12 || !r.u32at(offset + winf + 4, groups) || groups != 1 ||
                        !r.u32at(offset + winf + 8, group) || group > size || size - group < 0x74 ||
                        !r.u32at(offset + group + 0x70, count)) {
                        why = "a wave system is not a single-file one";
                        return false;
                    }
                    b.name.assign(reinterpret_cast<const char*>(&aaf[offset + group]), strnlen(reinterpret_cast<const char*>(&aaf[offset + group]), 0x70));
                    b.waves = count;
                }
                out.push_back(std::move(b));
            }
            continue;
        }
        if (type == 1 || (type >= 4 && type <= 8)) {
            u32 offset, size;
            if (!r.u32at(at, offset) || !r.u32at(at + 4, size) || offset > aaf.size() || size > aaf.size() - offset) {
                why = "a record lies outside the file";
                return false;
            }
            if (type != 4) out.push_back({type, 0, offset, size, "", 0});
            at += 12;
            continue;
        }
        do {
            u32 v;
            if (!r.u32at(at, v)) { why = "an unknown record is not terminated"; return false; }
            at += 4;
            if (v == 0) break;
        } while (true);
    }
    why = "too many records";
    return false;
}

bool same(const std::vector<u8>& a, const Blob& x, const std::vector<u8>& b, const Blob& y) {
    return x.size == y.size && std::memcmp(&a[x.offset], &b[y.offset], x.size) == 0;
}

// One changed bank, checked: where its wave infos come from.
struct Patch {
    u32 bank = 0;               // the guest TSimpleWaveBank
    const Swap::File* file = nullptr;
    bool loaded = false;        // its file is in ARAM now
    u32 aram = 0, room = 0, size = 0;
    struct Info {
        u32 handle;             // guest address of the TWaveHandle
        const u8* raw;          // the new table's 0x2C bytes
    };
    std::vector<Info> infos;
};

enum class Result { Done, Retry, Fail };

Result plan(const Swap& s, std::vector<Patch>& patches, const char** why) {
    static std::string reason;
    std::vector<Blob> oldB, newB;
    if (!parseAaf(s.oldAaf, oldB, reason) || !parseAaf(s.newAaf, newB, reason)) { *why = reason.c_str(); return Result::Fail; }
    if (oldB.size() != newB.size()) { *why = "the init files have different records"; return Result::Fail; }
    const u32 table = rd32(WAVEBANK_TABLE), tableSize = rd32(WAVEBANK_COUNT);
    if (!table || !tableSize) { *why = "the game's audio is not up"; return Result::Retry; }
    const Be nb{s.newAaf};
    for (size_t k = 0; k < newB.size(); ++k) {
        const Blob &o = oldB[k], &n = newB[k];
        if (o.type != n.type || o.index != n.index) { *why = "the init files have different records"; return Result::Fail; }
        if (same(s.oldAaf, o, s.newAaf, n)) continue;
        if (n.type != 3) { reason = "record type " + std::to_string(n.type) + " differs (only wave systems may)"; *why = reason.c_str(); return Result::Fail; }
        if (o.name != n.name || o.waves != n.waves) { *why = "a changed wave system is another file or has other waves"; return Result::Fail; }
        const Swap::File* file = nullptr;
        for (const Swap::File& f : s.files) {
            if (f.name.size() != n.name.size()) continue;
            bool eq = true;
            for (size_t i = 0; i < f.name.size(); ++i) eq &= (f.name[i] | 0x20) == (n.name[i] | 0x20);
            if (eq) file = &f;
        }
        if (!file || file->bytes.empty()) { reason = n.name + " changed but its file was not given"; *why = reason.c_str(); return Result::Fail; }
        if (n.index >= tableSize) { *why = "wave system out of range"; return Result::Fail; }
        Patch p;
        p.bank = rd32(table + 4 * n.index);
        p.file = file;
        const u32 count = p.bank ? rd32(p.bank + BANK_WAVE_COUNT) : 0, waveTable = p.bank ? rd32(p.bank + BANK_WAVE_TABLE) : 0;
        if (!p.bank || !count || !waveTable) { reason = n.name + "'s bank is not registered"; *why = reason.c_str(); return Result::Retry; }
        // The new table: JASWSParser::createSimpleWaveBank's walk.
        const size_t w = n.offset;
        u32 arcBank, ctrlGroup, scene, ctrl, waves, archive;
        if (!nb.u32at(w + 0x10, arcBank) || !nb.u32at(w + 0x14, ctrlGroup)) { *why = "a wave system is damaged"; return Result::Fail; }
        if (!nb.u32at(w + ctrlGroup + 0xC, scene) || !nb.u32at(w + scene + 0xC, ctrl) || !nb.u32at(w + ctrl + 4, waves) || waves != n.waves ||
            !nb.u32at(w + arcBank + 8, archive)) { *why = "a wave system is damaged"; return Result::Fail; }
        for (u32 i = 0; i < waves; ++i) {
            u32 ctrlWave, id, rawOff;
            if (!nb.u32at(w + ctrl + 8 + 4 * i, ctrlWave) || !nb.u32at(w + ctrlWave, id) || !nb.u32at(w + archive + 0x74 + 4 * i, rawOff)) {
                *why = "a wave entry is damaged";
                return Result::Fail;
            }
            id &= 0xFFFF;
            if (id >= count || rawOff > n.size || n.size - rawOff < INFO_SIZE) { *why = "a wave id or entry is outside the bank"; return Result::Fail; }
            p.infos.push_back({waveTable + id * HANDLE_STRIDE, &s.newAaf[w + rawOff]});
        }
        // The file.
        const u32 state = rd32(p.bank + BANK_ARC_STATE), room = rd32(p.bank + BANK_SIZE);
        p.size = u32(file->bytes.size());
        if (state == 1) { *why = "the wave file is loading"; return Result::Retry; }
        if (state == 2) {
            p.loaded = true;
            p.aram = rd32(p.bank + BANK_ARAM_BASE);
            p.room = room;
            if (!p.aram || !room || u64(p.aram) + room > kAramSize || p.size > (4u << 20)) {
                reason = n.name + "'s ARAM block is not as expected";
                *why = reason.c_str();
                return Result::Fail;
            }
            if (zeldaAramInUse(p.aram, p.aram + room)) { *why = "a voice of it is playing"; return Result::Retry; }
        }
        patches.push_back(std::move(p));
    }
    return Result::Done;
}

void patchInfos(const Patch& p) {
    for (const Patch::Info& i : p.infos) {
        // (JASWSParser: the fields as they are in the table, big-endian both sides; the arc pointer stays)
        u8* dst = hostp(i.handle + HANDLE_INFO);
        std::memcpy(dst, i.raw, INFO_ARC_PTR);
        std::memcpy(dst + INFO_ARC_PTR + 4, i.raw + INFO_ARC_PTR + 4, INFO_SIZE - INFO_ARC_PTR - 4);
    }
}

// False when a bank could not be given its new block (its old one is back, with its old waves).
bool apply(Ctx& c, std::vector<Patch>& patches) {
    bool allOk = true;
    for (Patch& p : patches) {
        if (!p.loaded) {
            patchInfos(p);
            wr32(p.bank + BANK_SIZE, p.size);  // read at its next load
            log("[voice] %s: %zu wave entries replaced, %u bytes (not loaded: read at its next load)\n", p.file->name.c_str(),
                p.infos.size(), p.size);
            continue;
        }
        const std::vector<u8> oldData(aramPtr(p.aram), aramPtr(p.aram) + p.room);
        std::vector<std::array<u8, INFO_SIZE>> oldInfos;
        for (const Patch::Info& i : p.infos) {
            std::array<u8, INFO_SIZE> a;
            std::memcpy(a.data(), hostp(i.handle + HANDLE_INFO), INFO_SIZE);
            oldInfos.push_back(a);
        }
        const Ctx saved = c;
        const auto call = [&](u32 fn, std::initializer_list<u32> args) { return guestCall(c, fn, args); };
        call(FN_ARC_ERASE, {p.bank + 4});
        wr32(p.bank + BANK_SIZE, p.size);
        const bool ok = (call(FN_HEAP_ALLOC, {p.bank + BANK_HEAP, ARAM_ROOT_HEAP, p.size}) & 0xFF) != 0;
        if (!ok) {  // no room for the new one: the old size is free again
            wr32(p.bank + BANK_SIZE, p.room);
            if ((call(FN_HEAP_ALLOC, {p.bank + BANK_HEAP, ARAM_ROOT_HEAP, p.room}) & 0xFF) == 0)
                log("[voice] %s: the old block could not be taken again; the wave bank stays unloaded\n", p.file->name.c_str());
        }
        const u32 base = rd32(p.bank + BANK_ARAM_BASE);
        if (base && u64(base) + std::max<size_t>(oldData.size(), p.size) <= kAramSize) {
            if (ok) {
                std::memcpy(aramPtr(base), p.file->bytes.data(), p.size);
                patchInfos(p);
            } else {
                std::memcpy(aramPtr(base), oldData.data(), oldData.size());
                for (size_t k = 0; k < p.infos.size(); ++k)
                    std::memcpy(hostp(p.infos[k].handle + HANDLE_INFO), oldInfos[k].data(), INFO_SIZE);
            }
            wr32(p.bank + BANK_LOADED, 1);
            wr32(p.bank + BANK_ARC_STATE, 2);
        }
        c = saved;
        log("[voice] %s: %s (%zu wave entries, %u bytes, ARAM %08X -> %08X)\n", p.file->name.c_str(),
            ok ? "swapped" : "no room for it in ARAM, the old one is back", p.infos.size(), p.size, p.aram, base);
        allOk &= ok;
    }
    return allOk;
}
}  // namespace

bool voiceSwapLive() { return s_live; }

bool voiceSwapRequest(const void* oldAaf, size_t oldN, const void* newAaf, size_t newN, const char* const* names,
                      const void* const* data, const size_t* sizes, int count) {
    if (!oldAaf || !newAaf || !oldN || !newN) return false;
    auto s = std::make_unique<Swap>();
    s->oldAaf.assign(static_cast<const u8*>(oldAaf), static_cast<const u8*>(oldAaf) + oldN);
    s->newAaf.assign(static_cast<const u8*>(newAaf), static_cast<const u8*>(newAaf) + newN);
    for (int i = 0; i < count; ++i)
        s->files.push_back({names[i], std::vector<u8>(static_cast<const u8*>(data[i]), static_cast<const u8*>(data[i]) + sizes[i])});
    std::lock_guard<std::mutex> g(s_lock);
    s_pending = std::move(s);
    s_state = 1;
    return true;
}

int voiceSwapState() { return s_state; }

// Each frame, at the start of mDoAud_Execute (a hook of bod_host.cpp's: on the game thread in game code, so the guest calls
// are made here).
void voiceSwapFrame(Ctx& c) {
    if (!s_live && rd32(WAVEBANK_TABLE) != 0 && rd32(WAVEBANK_COUNT) != 0) s_live = true;
    if (s_state != 1) return;
    static unsigned s_every;
    if (++s_every % 6 != 0) return;  // (a try parses the files again)
    std::unique_ptr<Swap> mine;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_pending) return;
        mine = std::move(s_pending);
    }
    std::vector<Patch> patches;
    const char* why = "";
    const Result r = plan(*mine, patches, &why);
    if (r == Result::Retry) {
        if (++s_wait <= kWaitFrames / 6) {
            std::lock_guard<std::mutex> g(s_lock);
            if (!s_pending) s_pending = std::move(mine);  // (a newer request replaces it)
            return;
        }
        log("[voice] not swapped: %s for %d frames; the voice changes at the next start\n", why, kWaitFrames);
        s_wait = 0;
        s_state = -1;
        return;
    }
    s_wait = 0;
    if (r == Result::Fail) {
        log("[voice] not swapped: %s; the voice changes at the next start\n", why);
        s_state = -1;
        return;
    }
    const bool ok = apply(c, patches);
    log("[voice] %s: %zu wave bank%s\n", ok ? "swapped" : "not swapped", patches.size(), patches.size() == 1 ? "" : "s");
    s_state = ok ? 2 : -1;
}

}  // namespace rt::bod
