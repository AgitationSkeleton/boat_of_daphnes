#include "setup/firstrun.h"

#include "disc/discimage.h"
#include "recompiler/build.h"
#include "runtime/abi.h"
#include "setup/sha1.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifndef BOD_VERSION
#define BOD_VERSION "dev"
#endif

namespace setup {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

struct EmbeddedFile {
    const char* path;
    const unsigned char* data;
    size_t size;
};
#include "setup_files.inc"  // the module's headers and glue, the name list (tools/setup/embed.cmake)

std::string embedded(const char* path) {
    for (const EmbeddedFile& f : kEmbedded)
        if (!std::strcmp(f.path, path)) return std::string(reinterpret_cast<const char*>(f.data), f.size);
    return {};
}

constexpr const char* kGameId = "GZLE01";
// The program the recompiler is made for: main.dol of The Wind Waker (USA, revision 0). The disc image may be any
// dump of it (.iso, .nkit.iso, .ciso, .gcz, .rvz, .wia): the program is what counts.
constexpr const char* kDolSha1 = "8d28bab68bb5078c38e43f29206f0bd01f7e7a67";
// ... and wwrando 1.10.0's reference run of it (src/runtime/bod_rando.cpp's site tables are made for this one).
constexpr const char* kRandoDolSha1 = "d05afaa1a6ca0c19ccde1a8fcb3e3e2a2a794524";
constexpr const char* kRandoSeed = "RV1";
// Each step's share of the bar: about its share of the time.
constexpr float kWeightGame[FirstRun::kSteps] = {0.02f, 0.01f, 0.05f, 0.91f, 0.01f, 0, 0, 0, 0, 0};
constexpr float kWeightBoth[FirstRun::kSteps] = {0.01f, 0.005f, 0.02f, 0.43f, 0.005f, 0.04f, 0.025f, 0.43f, 0.005f, 0.03f};
constexpr const char* kStampGame = "tww_game.txt";
constexpr const char* kStampRando = "tww_game_rando.txt";

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
std::string fmtBytes(uint64_t n) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f MB", double(n) / 1048576.0);
    return b;
}

// ---- the disc: its file table and files ----
struct Disc {
    std::unique_ptr<disc::Image> img;
    Bytes header, fst;
    struct File {
        std::string path;
        uint32_t offset, size;
    };
    std::vector<File> files;
    bool open(const fs::path& p, std::string& err) {
        img = disc::open(p, err);
        if (!img) return false;
        header.resize(0x440);
        if (!img->read(0, header.data(), header.size())) return err = "can't read its header", false;
        const uint32_t fo = be32(&header[0x424]), fsz = be32(&header[0x428]);
        if (!fsz || fsz > (8u << 20)) return err = "its file table is not a GameCube one", false;
        fst.resize(fsz);
        if (!img->read(fo, fst.data(), fsz)) return err = "can't read its file table", false;
        const uint32_t n = be32(&fst[8]);
        const size_t strs = size_t(n) * 12;
        if (strs >= fst.size()) return err = "its file table is damaged", false;
        auto name = [&](uint32_t i) { return std::string(reinterpret_cast<const char*>(&fst[strs + (be32(&fst[i * 12]) & 0xFFFFFF)])); };
        std::function<void(uint32_t, uint32_t, const std::string&)> walk = [&](uint32_t i, uint32_t end, const std::string& prefix) {
            while (i < end) {
                if (fst[i * 12] != 0) {
                    const uint32_t next = be32(&fst[i * 12 + 8]);
                    walk(i + 1, next, prefix + name(i) + "/");
                    i = next;
                } else {
                    files.push_back({prefix + name(i), be32(&fst[i * 12 + 4]), be32(&fst[i * 12 + 8])});
                    i++;
                }
            }
        };
        walk(1, n, "");
        return true;
    }
    std::string gameId() const { return std::string(reinterpret_cast<const char*>(header.data()), 6); }
    int revision() const { return header.size() > 7 ? header[7] : -1; }
    bool read(const File& f, Bytes& out) {
        out.resize(f.size);
        return img->read(f.offset, out.data(), f.size);
    }
    const File* find(const std::string& path) const {
        for (const File& f : files) {
            if (f.path.size() != path.size()) continue;
            bool same = true;
            for (size_t i = 0; same && i < path.size(); i++) same = std::tolower(uint8_t(f.path[i])) == std::tolower(uint8_t(path[i]));
            if (same) return &f;
        }
        return nullptr;
    }
    bool readDol(Bytes& out) {
        const uint32_t at = be32(&header[0x420]);
        uint8_t dh[0x100];
        if (!img->read(at, dh, sizeof dh)) return false;
        uint32_t size = 0x100;
        for (int i = 0; i < 18; i++) size = std::max(size, be32(dh + i * 4) + be32(dh + 0x90 + i * 4));
        out.resize(size);
        return img->read(at, out.data(), size);
    }
};

// Nintendo's Yaz0 (tools/extract.py's yaz0): a 16-byte header, then groups of a flag byte and 8 items.
Bytes yaz0(const Bytes& d) {
    if (d.size() < 16 || std::memcmp(d.data(), "Yaz0", 4) != 0) return d;
    const uint32_t size = be32(&d[4]);
    Bytes out(size);
    size_t src = 16, dst = 0;
    while (dst < size && src < d.size()) {
        const uint8_t flags = d[src++];
        for (int bit = 7; bit >= 0 && dst < size && src < d.size(); bit--) {
            if (flags >> bit & 1) {
                out[dst++] = d[src++];
            } else {
                if (src + 1 >= d.size()) return out;
                const uint8_t b1 = d[src], b2 = d[src + 1];
                src += 2;
                const size_t dist = ((b1 & 0x0F) << 8 | b2) + 1;
                size_t n = b1 >> 4;
                if (n == 0) n = (src < d.size() ? d[src++] : 0) + 0x12;
                else n += 2;
                for (size_t k = 0; k < n && dst < size; k++, dst++) out[dst] = dst >= dist ? out[dst - dist] : 0;
            }
        }
    }
    return out;
}

// A RARC archive's files (tools/extract.py's rarc): (name, bytes), directories walked flat.
std::vector<std::pair<std::string, Bytes>> rarc(const Bytes& d) {
    std::vector<std::pair<std::string, Bytes>> out;
    if (d.size() < 0x40 || std::memcmp(d.data(), "RARC", 4) != 0) return out;
    const uint32_t dataOff = be32(&d[0x0C]) + 0x20, info = 0x20;
    const uint32_t nEnts = be32(&d[info + 8]), entsOff = be32(&d[info + 12]) + info, strsOff = be32(&d[info + 20]) + info;
    for (uint32_t i = 0; i < nEnts; i++) {
        const size_t e = entsOff + size_t(i) * 20;
        if (e + 16 > d.size()) break;
        const uint16_t type = be16(&d[e + 4]), nameOff = be16(&d[e + 6]);
        const uint32_t off = be32(&d[e + 8]), size = be32(&d[e + 12]);
        if (type & 0x0200) continue;  // a directory
        const char* nm = reinterpret_cast<const char*>(&d[strsOff + nameOff]);
        if (size_t(dataOff) + off + size > d.size()) continue;
        out.push_back({nm, Bytes(d.begin() + dataOff + off, d.begin() + dataOff + off + size)});
    }
    return out;
}

std::string sha1Of(const Bytes& b) {
    Sha1 h;
    h.add(b.data(), b.size());
    return h.hex();
}

// tww_game.txt: what the module beside it was built by and from.
struct Stamp {
    std::string version, sha1;
    bool operator==(const Stamp& o) const = default;
};
Stamp readStamp(const fs::path& p) {
    Stamp s;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        if (l.substr(0, eq) == "version") s.version = l.substr(eq + 1);
        else if (l.substr(0, eq) == "sha1") s.sha1 = l.substr(eq + 1);
    }
    return s;
}
std::string buildVersion() { return std::string(BOD_VERSION) + "/abi" + std::to_string(SA2B_GAME_ABI); }
void writeStamp(const fs::path& p, const std::string& sha1) { std::ofstream(p) << "version=" << buildVersion() << "\nsha1=" << sha1 << "\n"; }

// ---- child processes (the compiler, the linker, the randomizer's Python) ----
struct Child {
    HANDLE proc = nullptr;
    size_t index = 0;
    fs::path log;
};
bool spawn(HANDLE job, const fs::path& exe, const std::wstring& args, const fs::path& dir, const fs::path& logPath, Child& c, std::string& err,
           const wchar_t* env = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return err = "can't write " + u8(logPath), false;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> buf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    si.lpAttributeList = attrs;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                                       (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                                   const_cast<wchar_t*>(env), dir.c_str(), &si.StartupInfo, &pi);
    const DWORD e = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(out);
    if (!ok) return err = "couldn't start " + u8(exe.filename()) + " (error " + std::to_string(e) + ")", false;
    AssignProcessToJobObject(job, pi.hProcess);  // ends with the game, whatever happens to it
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c.proc = pi.hProcess;
    c.log = logPath;
    return true;
}
HANDLE makeJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    return job;
}
std::string readText(const fs::path& p, size_t maxLines) {
    std::ifstream f(p);
    std::string out;
    size_t n = 0;
    for (std::string l; n < maxLines && std::getline(f, l); n++) out += l + "\n";
    return out;
}
std::string tailText(const fs::path& p, size_t maxLines) {
    std::ifstream f(p);
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    std::string out;
    for (size_t i = lines.size() > maxLines ? lines.size() - maxLines : 0; i < lines.size(); i++) out += lines[i] + "\n";
    return out;
}

fs::path toolchain(const fs::path& exeDir) { return exeDir / "tools" / "llvm"; }
fs::path randoDir(const fs::path& exeDir) { return exeDir / "rando"; }
bool haveRando(const fs::path& exeDir) {
    std::error_code ec;
    return fs::exists(randoDir(exeDir) / "python" / "python.exe", ec) && fs::exists(randoDir(exeDir) / "patch_game.py", ec);
}

// ---- the two programs' functions that differ (tools/rando/module_diff.py) ----
// The recompiler's text of each function, with the per-module table indexes left out (an override slot or a hook
// number says nothing about what the code does): the functions whose text differs, or that one program has alone.
std::map<std::pair<uint32_t, uint32_t>, std::string> functionTexts(const fs::path& gen) {
    static const std::regex head(R"(^void (f_([0-9A-F]{8})|m(\d+)_([0-9A-F]{8}))(_orig)?\(Ctx& c\) \{$)");
    static const std::regex numbers(R"((g_fnOverride|g_hooks|g_patchPoints)\[\d+\]|runOverride\(c, \d+u\))");
    std::map<std::pair<uint32_t, uint32_t>, std::string> out;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(gen, ec))
        if (e.path().extension() == ".cpp" && e.path().filename().string().find("_table.cpp") == std::string::npos) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        std::ifstream f(p);
        std::pair<uint32_t, uint32_t> key{0, 0};
        bool in = false;
        std::string body;
        for (std::string line; std::getline(f, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!in) {
                std::smatch m;
                if (line.rfind("void ", 0) == 0 && std::regex_match(line, m, head)) {
                    key = m[2].matched ? std::pair<uint32_t, uint32_t>{0, uint32_t(std::stoul(m[2].str(), nullptr, 16))}
                                       : std::pair<uint32_t, uint32_t>{uint32_t(std::stoul(m[3].str())), uint32_t(std::stoul(m[4].str(), nullptr, 16))};
                    std::string h = line;
                    if (const size_t o = h.find("_orig("); o != std::string::npos) h.replace(o, 6, "(");
                    body = h + "\n";
                    in = true;
                }
                continue;
            }
            if (line.find("g_fnOverride[") != std::string::npos || line.find("g_hooks[") != std::string::npos ||
                line.find("runOverride(") != std::string::npos || line.find("g_patchPoints[") != std::string::npos)
                body += std::regex_replace(line, numbers, "$1") + "\n";
            else
                body += line + "\n";
            if (line == "}") {
                out[key] += body;
                in = false;
            }
        }
    }
    return out;
}

}  // namespace

struct FirstRun::Program {
    Bytes dol;
    std::vector<std::pair<std::string, Bytes>> rels;
};

const char* FirstRun::stepName(int s) {
    static const char* names[kSteps] = {"Checking your disc",          "Reading the game's program",  "Recompiling it to C++",
                                        "Compiling the game",          "Linking the game",            "Making the randomizer's game",
                                        "Recompiling the randomizer",  "Compiling the randomizer",    "Linking the randomizer",
                                        "Finishing (colours, sails, the randomizer's switch)"};
    return s >= 0 && s < kSteps ? names[s] : "";
}

FirstRun::~FirstRun() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

bool FirstRun::looksRight(const fs::path& path, std::string& why) {
    Disc d;
    if (!d.open(path, why)) return why = "That file is not a GameCube disc image (" + why + ").", false;
    if (d.gameId() != kGameId)
        return why = "That disc image is " + d.gameId() + ". Boat of Daphnes plays The Legend of Zelda: The Wind Waker for the GameCube (USA), GZLE01.", false;
    if (d.revision() != 0) return why = "That disc image is a later revision of The Wind Waker (USA). The recompiler is made for the first (revision 0).", false;
    return true;
}

bool FirstRun::needed(const fs::path& exeDir, const fs::path& disc) {
    (void)disc;
    std::error_code ec;
    if (!fs::exists(exeDir / "tww_game.dll", ec) || readStamp(exeDir / kStampGame).version != buildVersion()) return true;
    if (haveRando(exeDir) && (!fs::exists(exeDir / "tww_game_rando.dll", ec) || readStamp(exeDir / kStampRando).version != buildVersion()))
        return true;
    return false;
}

void FirstRun::start(const fs::path& exeDir, const fs::path& disc) {
    if (m_running) return;
    if (m_thread.joinable()) m_thread.join();
    m_exeDir = exeDir, m_disc = disc, m_logFile = exeDir / "setup.log";
    const char* la = std::getenv("LOCALAPPDATA");
    m_work = la ? fs::path(std::u8string(reinterpret_cast<const char8_t*>(la))) / "BoatOfDaphnes" / "build" : exeDir / "setup_work";
    {
        std::lock_guard g(m_lock);
        m_status = {};
        m_status.running = true;
        m_status.rando = haveRando(exeDir);
    }
    m_cancel = false;
    m_running = true;
    m_t0 = std::chrono::steady_clock::now();
    std::ofstream(m_logFile, std::ios::trunc);
    m_thread = std::thread([this] { run(); });
}

void FirstRun::cancel() { m_cancel = true; }

FirstRun::Status FirstRun::status() const {
    std::lock_guard g(m_lock);
    Status s = m_status;
    s.elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_t0).count();
    if (s.running && s.overall > 0.05f && s.elapsed > 10) s.eta = s.elapsed * (1 - s.overall) / s.overall;
    return s;
}

void FirstRun::set(int step, float frac, const std::string& detail) {
    std::lock_guard g(m_lock);
    const float* w = m_status.rando ? kWeightBoth : kWeightGame;
    m_status.step = step;
    m_status.stepFrac = std::clamp(frac, 0.f, 1.f);
    float o = 0;
    for (int i = 0; i < step; i++) o += w[i];
    m_status.overall = std::min(1.f, o + w[step] * m_status.stepFrac);
    m_status.detail = detail;
}

bool FirstRun::fail(const std::string& why) {
    log("FAILED: " + why);
    std::lock_guard g(m_lock);
    if (m_cancel) m_status.cancelled = true;
    else m_status.failed = true, m_status.error = why;
    return false;
}

void FirstRun::log(const std::string& line) {
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_t0).count();
    char b[32];
    std::snprintf(b, sizeof b, "%7.1f s  ", t);
    std::ofstream(m_logFile, std::ios::app) << b << line << "\n";
}

void FirstRun::run() {
    log("Boat of Daphnes setup " BOD_VERSION ": building the game from " + u8(m_disc));
    std::error_code ec;
    bool ok = false;
    do {
        {  // the program's folder must take the modules (not so in Program Files, say)
            const fs::path probe = m_exeDir / "bod_write_test.tmp";
            std::ofstream(probe) << "1";
            if (!fs::exists(probe, ec)) {
                fail("The game's folder can't be written to (" + u8(m_exeDir) + "). Move Boat of Daphnes to a folder of your own, such as Documents, "
                     "and start it again.");
                break;
            }
            fs::remove(probe, ec);
        }
        const fs::path tools = toolchain(m_exeDir);
        if (!fs::exists(tools / "clang.exe", ec) || !fs::exists(tools / "lld-link.exe", ec) || !fs::exists(m_exeDir / "boat-of-daphnes.lib", ec)) {
            fail("Files the setup needs are missing (tools/llvm, boat-of-daphnes.lib). Extract the whole download again.");
            break;
        }
        fs::remove_all(m_work, ec);
        fs::create_directories(m_work, ec);
        // The game: built unless the one there was built by this version.
        if (!fs::exists(m_exeDir / "tww_game.dll", ec) || readStamp(m_exeDir / kStampGame).version != buildVersion()) {
            std::string sha1;
            if (!buildGame(Check, m_disc, "tww_game.dll", "gen", false, sha1)) break;
            writeStamp(m_exeDir / kStampGame, sha1);
        }
        // The randomizer, when its generator is here.
        if (haveRando(m_exeDir) && (!fs::exists(m_exeDir / "tww_game_rando.dll", ec) || readStamp(m_exeDir / kStampRando).version != buildVersion())) {
            std::string sha1;
            if (!fs::exists(m_work / "gen", ec) && !buildGame(Check, m_disc, nullptr, "gen", false, sha1)) break;
            if (!patchRando()) break;
            if (!buildGame(RandoPatch, m_exeDir / "game" / "rando" / "GZLE99.iso", "tww_game_rando.dll", "gen_rando", true, sha1)) break;
            if (!extras()) break;
            writeStamp(m_exeDir / kStampRando, sha1);
        }
        ok = !m_cancel;
    } while (false);
    if (!ok && m_cancel) log("cancelled");
    fs::remove_all(m_work, ec);  // the work files (some GB) are never kept: a failed build starts over
    {
        std::lock_guard g(m_lock);
        m_status.running = false;
        m_status.done = ok;
        if (!ok && m_cancel) m_status.cancelled = true, m_status.failed = false;
        if (ok) m_status.overall = 1, m_status.stepFrac = 1;
    }
    log(ok ? "done" : "stopped");
    m_running = false;
}

// Check (or the randomizer's patch step), extract, recompile, compile, link one program into `dll`.
bool FirstRun::buildGame(int base, const fs::path& disc, const char* dll, const char* gen, bool rando, std::string& sha1) {
    const bool textOnly = dll == nullptr;  // (the recompiled text only: what the randomizer's switch compares)
    std::error_code ec;
    Program p;
    if (!extract(rando ? RandoPatch : Extract, disc, p)) return false;
    sha1 = sha1Of(p.dol);
    if (sha1 != (rando ? kRandoDolSha1 : kDolSha1))
        return fail(rando ? "The randomizer's game came out different from the one Boat of Daphnes is made for (main.dol SHA-1 " + sha1 + ")."
                          : "This disc image's program is not the one Boat of Daphnes is made for: The Wind Waker (USA, GZLE01, revision 0). "
                            "Its main.dol SHA-1 is " + sha1 + ", expected " + kDolSha1 + ".");
    log(std::string(dll ? dll : gen) + ": main.dol SHA-1 " + sha1 + ", " + std::to_string(p.rels.size()) + " code modules");
    const fs::path genDir = m_work / gen, objDir = m_work / (std::string(gen) + "_obj");
    fs::create_directories(genDir, ec);
    if (!recompile(rando ? RandoRecompile : Recompile, p, genDir)) return false;
    p = {};
    if (textOnly) return true;
    if (!compile(rando ? RandoCompile : Compile, genDir, objDir, rando)) return false;
    if (!link(rando ? RandoLink : Link, objDir, dll)) return false;
    fs::copy_file(objDir / dll, m_exeDir / dll, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("Couldn't put " + std::string(dll) + " in the game's folder: " + ec.message() + ".");
    (void)base;
    return true;
}

// main.dol and every code module: the loose Yaz0 ones in rels/ and the ones packed in RELS.arc.
bool FirstRun::extract(int step, const fs::path& path, Program& p) {
    set(step == Extract ? Check : step, 0, "");
    Disc d;
    std::string err;
    if (!d.open(path, err)) return fail("Can't read the disc image " + u8(path.filename()) + ": " + err + ".");
    if (step == Extract) {
        if (d.gameId() != kGameId) return fail("This disc image is " + d.gameId() + ", not The Wind Waker (USA, GZLE01).");
        set(Check, 1, d.gameId() + " (" + d.img->format() + ")");
        set(Extract, 0, "");
    }
    if (!d.readDol(p.dol)) return fail("Can't read the game's program from " + u8(path.filename()) + ".");
    size_t i = 0;
    for (const Disc::File& f : d.files) {
        if (m_cancel) return fail("cancelled");
        std::string low = f.path;
        for (char& c : low) c = char(std::tolower(uint8_t(c)));
        Bytes raw;
        if (low.rfind("rels/", 0) == 0 && low.size() > 9 && low.ends_with(".rel")) {
            if (!d.read(f, raw)) return fail("Can't read " + f.path + " from the disc image.");
            p.rels.emplace_back(f.path.substr(5), yaz0(raw));
        } else if (low == "rels.arc") {
            if (!d.read(f, raw)) return fail("Can't read RELS.arc from the disc image.");
            for (auto& [name, data] : rarc(raw))
                if (name.size() > 4 && (name.ends_with(".rel") || name.ends_with(".REL"))) p.rels.emplace_back(name, yaz0(data));
        }
        set(step == RandoPatch ? RandoPatch : Extract, float(++i) / float(d.files.size()), std::to_string(p.rels.size()) + " code modules");
    }
    std::sort(p.rels.begin(), p.rels.end(), [](auto& a, auto& b) { return a.first < b.first; });
    // (a module in both places: the first, as tools/extract.py writes them over each other in the same order)
    p.rels.erase(std::unique(p.rels.begin(), p.rels.end(), [](auto& a, auto& b) { return a.first == b.first; }), p.rels.end());
    return true;
}

// PowerPC -> C++, in this process (src/recompiler).
bool FirstRun::recompile(int step, Program& p, const fs::path& gen) {
    set(step, 0, "Finding every function");
    sa2rc::Input in;
    in.dol = std::move(p.dol);
    in.rels = std::move(p.rels);
    in.names = embedded("names.txt");
    sa2rc::Program prog;
    std::string err, notes;
    if (!sa2rc::buildProgram(in, prog, err, &notes, [this, step](float f) { set(step, 0.8f * f, "Finding every function"); }))
        return fail("The recompiler couldn't read the game's program: " + err);
    if (!notes.empty()) log(notes.substr(0, notes.size() - 1));
    if (m_cancel) return fail("cancelled");
    set(step, 0.8f, "Translating to C++");
    sa2rc::EmitStats st;
    std::vector<sa2rc::OutputFile> files = sa2rc::emitProgram(prog, st);
    log("recompiled " + std::to_string(st.functions) + " functions into " + std::to_string(files.size()) + " files");
    for (size_t i = 0; i < files.size(); i++) {
        if (m_cancel) return fail("cancelled");
        std::ofstream o(gen / files[i].name, std::ios::binary);
        o.write(files[i].text.data(), std::streamsize(files[i].text.size()));
        if (!o) return fail("Couldn't write the recompiled code to " + u8(m_work) + " (is the drive full? It needs about 3 GB).");
        set(step, 0.95f + 0.05f * float(i + 1) / float(files.size()), std::to_string(i + 1) + " of " + std::to_string(files.size()) + " files written");
    }
    // what the module is compiled with besides: the runtime's headers and the glue
    for (const EmbeddedFile& f : kEmbedded) {
        const fs::path q = m_work / fs::path(f.path);
        if (q.extension() == ".txt") continue;
        std::error_code ec;
        fs::create_directories(q.parent_path(), ec);
        std::ofstream(q, std::ios::binary).write(reinterpret_cast<const char*>(f.data), std::streamsize(f.size));
    }
    return true;
}

// Every file with the bundled clang, as many at once as there are processor threads (and memory for them), largest
// first so the last ones finish together. The randomizer's module enters the runtime's overrides by its own name.
bool FirstRun::compile(int step, const fs::path& gen, const fs::path& objDir, bool rando) {
    set(step, 0, "");
    struct Src {
        fs::path path;
        uint64_t size;
    };
    std::vector<Src> srcs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(gen, ec))
        if (e.path().extension() == ".cpp") srcs.push_back({e.path(), e.file_size(ec)});
    srcs.push_back({m_work / "glue.cpp", fs::file_size(m_work / "glue.cpp", ec)});
    std::sort(srcs.begin(), srcs.end(), [](const Src& a, const Src& b) { return a.size > b.size; });
    uint64_t total = 0, done = 0;
    for (const Src& s : srcs) total += s.size;
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const uint64_t byFree = ms.ullAvailPhys / (400ull << 20), byInstalled = ms.ullTotalPhys / (2048ull << 20);
    const unsigned jobs = std::min({threads, unsigned(std::max<uint64_t>({1, byFree, byInstalled})), 32u});
    log("compiling " + std::to_string(srcs.size()) + " files (" + fmtBytes(total) + ") with " + std::to_string(jobs) + " at a time");
    fs::create_directories(objDir, ec);
    const fs::path clang = toolchain(m_exeDir) / "clang.exe";
    HANDLE job = makeJob();
    std::vector<Child> active;
    size_t next = 0, finished = 0;
    m_objects.clear();
    auto stopAll = [&] {
        for (Child& c : active) TerminateProcess(c.proc, 1), WaitForSingleObject(c.proc, 5000), CloseHandle(c.proc);
        active.clear();
        CloseHandle(job);
    };
    while (next < srcs.size() || !active.empty()) {
        if (m_cancel) return stopAll(), fail("cancelled");
        while (active.size() < jobs && next < srcs.size()) {
            const Src& s = srcs[next];
            const fs::path obj = objDir / s.path.filename().replace_extension(".obj");
            const std::wstring args = L"--target=x86_64-pc-windows-msvc -std=c++20 -O2 -g0 -ffreestanding -nostdinc -fno-exceptions -fno-rtti "
                                      L"-fno-threadsafe-statics -DSA2B_GAME_DLL " +
                                      std::wstring(rando ? L"-DrunOverride=runOverrideRando " : L"") + L"-I\"" + (m_work / "include").wstring() +
                                      L"\" -I\"" + gen.wstring() + L"\" -w -c \"" + s.path.wstring() + L"\" -o \"" + obj.wstring() + L"\"";
            Child c;
            std::string err;
            if (!spawn(job, clang, args, m_work, objDir / s.path.filename().replace_extension(".log"), c, err)) return stopAll(), fail(err);
            c.index = next++;
            active.push_back(c);
            m_objects.push_back(obj);
        }
        std::vector<HANDLE> hs;
        for (const Child& c : active) hs.push_back(c.proc);
        WaitForMultipleObjects(DWORD(hs.size()), hs.data(), FALSE, 250);
        for (size_t i = 0; i < active.size();) {
            if (WaitForSingleObject(active[i].proc, 0) != WAIT_OBJECT_0) {
                i++;
                continue;
            }
            DWORD code = 1;
            GetExitCodeProcess(active[i].proc, &code);
            CloseHandle(active[i].proc);
            const Child c = active[i];
            active.erase(active.begin() + ptrdiff_t(i));
            if (code != 0) {
                log("compiler error in " + u8(srcs[c.index].path.filename()) + ":\n" + readText(c.log, 12));
                stopAll();
                return fail("The compiler stopped on " + u8(srcs[c.index].path.filename()) + ". The details are in setup.log.");
            }
            done += srcs[c.index].size;
            finished++;
        }
        set(step, float(double(done) / double(total)), std::to_string(finished) + " of " + std::to_string(srcs.size()) + " files");
    }
    CloseHandle(job);
    log("compiled");
    return true;
}

// The module: no C library, no entry point; the runtime's functions from boat-of-daphnes.lib.
bool FirstRun::link(int step, const fs::path& objDir, const char* dll) {
    set(step, 0, "");
    {
        std::ofstream rsp(objDir / "objs.rsp");
        for (const fs::path& o : m_objects) rsp << "\"" << o.generic_string() << "\"\n";
    }
    const std::wstring args = L"/nologo /dll /noentry /nodefaultlib /machine:x64 /opt:ref /opt:noicf /out:\"" + (objDir / dll).wstring() +
                              L"\" @\"" + (objDir / "objs.rsp").wstring() + L"\" \"" + (m_exeDir / "boat-of-daphnes.lib").wstring() + L"\"";
    HANDLE job = makeJob();
    Child c;
    std::string err;
    if (!spawn(job, toolchain(m_exeDir) / "lld-link.exe", args, m_work, objDir / "link.log", c, err)) return CloseHandle(job), fail(err);
    const auto t0 = std::chrono::steady_clock::now();
    while (WaitForSingleObject(c.proc, 100) == WAIT_TIMEOUT) {
        if (m_cancel) {
            TerminateProcess(c.proc, 1);
            CloseHandle(c.proc), CloseHandle(job);
            return fail("cancelled");
        }
        set(step, 1 - std::exp(-std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() / 2.f), "");
    }
    DWORD code = 1;
    GetExitCodeProcess(c.proc, &code);
    CloseHandle(c.proc), CloseHandle(job);
    if (code != 0) {
        log("linker error:\n" + readText(objDir / "link.log", 20));
        return fail("The linker stopped. The details are in setup.log.");
    }
    log("linked " + std::string(dll));
    set(step, 1, "");
    return true;
}

// wwrando's reference run (its code for every option, seed independent) of the player's disc: game/rando/GZLE99.iso.
// wwrando reads plain ISO images only: another kind (.ciso, .rvz, .gcz, .wia) is written out as one first, as game/GZLE01.iso, where
// the seed generator also finds the disc.
bool FirstRun::patchRando() {
    std::error_code ec;
    set(RandoPatch, 0, "Preparing the disc for the randomizer");
    const fs::path gameDir = m_exeDir / "game";
    fs::create_directories(gameDir / "rando", ec);
    fs::path iso = gameDir / "GZLE01.iso";
    {
        std::string err;
        auto img = disc::open(m_disc, err);
        if (!img) return fail("Can't read the disc image: " + err + ".");
        if (const std::string kind = img->format(); kind == "ISO" || kind == "NKIT") {  // (an NKit disc has a file table of its own: wwrando reads it whole)
            iso = m_disc;
        } else if (!fs::exists(iso, ec) || fs::file_size(iso, ec) != img->size()) {
            std::ofstream out(iso, std::ios::binary);
            std::vector<char> buf(8 << 20);
            for (uint64_t at = 0; at < img->size(); at += buf.size()) {
                if (m_cancel) return fail("cancelled");
                const size_t n = size_t(std::min<uint64_t>(buf.size(), img->size() - at));
                if (!img->read(at, buf.data(), n)) return fail("Couldn't read the whole disc image.");
                out.write(buf.data(), std::streamsize(n));
                set(RandoPatch, 0.3f * float(double(at + n) / double(img->size())), "Writing a plain copy of the disc for the randomizer");
            }
            if (!out) return fail("Couldn't write " + u8(iso) + " (is the drive full? It needs 1.4 GB).");
        }
    }
    const fs::path rando = randoDir(m_exeDir);
    const std::wstring args = L"\"" + (rando / "patch_game.py").wstring() + L"\" --iso \"" + iso.wstring() + L"\" --out \"" +
                              (m_work / "ref").wstring() + L"\" --seed " + std::wstring(kRandoSeed, kRandoSeed + std::strlen(kRandoSeed)) +
                              L" --options \"" + (rando / "reference-options.json").wstring() + L"\" --files code --iso-out \"" +
                              (gameDir / "rando" / "GZLE99.iso").wstring() + L"\"";
    // its environment: ours, and where the generator's runtime is
    std::wstring env;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) env.append(p).push_back(L'\0');
        FreeEnvironmentStringsW(block);
    }
    env.append(L"RANDO_RUNTIME=" + rando.wstring()).push_back(L'\0');
    env.push_back(L'\0');
    HANDLE job = makeJob();
    Child c;
    std::string err;
    if (!spawn(job, rando / "python" / "python.exe", args, rando, m_work / "rando_patch.log", c, err, env.c_str())) return CloseHandle(job), fail(err);
    const auto t0 = std::chrono::steady_clock::now();
    while (WaitForSingleObject(c.proc, 200) == WAIT_TIMEOUT) {
        if (m_cancel) {
            TerminateProcess(c.proc, 1);
            CloseHandle(c.proc), CloseHandle(job);
            return fail("cancelled");
        }
        const float s = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        set(RandoPatch, 0.3f + 0.7f * (1 - std::exp(-s / 40.f)), "The Wind Waker Randomizer is patching the game");
    }
    DWORD code = 1;
    GetExitCodeProcess(c.proc, &code);
    CloseHandle(c.proc), CloseHandle(job);
    if (code != 0 || !fs::exists(gameDir / "rando" / "GZLE99.iso", ec)) {
        log("randomizer patch error:\n" + tailText(m_work / "rando_patch.log", 25));
        return fail("The randomizer couldn't make its game from your disc. The details are in setup.log.");
    }
    log("the randomizer's reference game made");
    return true;
}

// What switching between the two programs needs, and the Cosmetics Editor's and Swift Sail's data.
bool FirstRun::extras() {
    std::error_code ec;
    set(Extras, 0, "The functions the two programs have differently");
    {
        const auto a = functionTexts(m_work / "gen"), b = functionTexts(m_work / "gen_rando");
        std::set<std::pair<uint32_t, uint32_t>> keys;
        for (const auto& [k, v] : a) keys.insert(k);
        for (const auto& [k, v] : b) keys.insert(k);
        std::vector<std::pair<uint32_t, uint32_t>> differ;
        for (const auto& k : keys) {
            const auto ia = a.find(k), ib = b.find(k);
            if (ia == a.end() || ib == b.end() || ia->second != ib->second) differ.push_back(k);
        }
        std::ofstream f(m_exeDir / "tww_game_rando.diff", std::ios::binary);
        const uint32_t count = uint32_t(differ.size());
        f.write("MDIF", 4).write(reinterpret_cast<const char*>(&count), 4);
        for (const auto& [m, addr] : differ) f.write(reinterpret_cast<const char*>(&m), 4).write(reinterpret_cast<const char*>(&addr), 4);
        log(std::to_string(differ.size()) + " functions differ between the two programs");
        if (a.empty() || b.empty()) return fail("The recompiled code to compare was not there (see setup.log).");
    }
    set(Extras, 0.3f, "The disc files the two have differently");
    {
        Disc va, ra;
        std::string err;
        if (!va.open(m_disc, err) || !ra.open(m_exeDir / "game" / "rando" / "GZLE99.iso", err)) return fail("Can't read the discs to compare: " + err);
        std::ofstream list(m_exeDir / "game" / "rando" / "changed.txt");
        size_t changed = 0;
        Bytes x, y;
        for (const Disc::File& f : va.files) {
            if (m_cancel) return fail("cancelled");
            const Disc::File* g = ra.find(f.path);
            if (!g) continue;
            bool differs = f.size != g->size;
            if (!differs) {
                if (!va.read(f, x) || !ra.read(*g, y)) return fail("Can't read " + f.path + " to compare.");
                differs = x != y;
            }
            if (differs) list << f.path << "\n", changed++;
        }
        log(std::to_string(changed) + " files differ between the discs");
        // the randomizer's title logo, kept for the file select's quest picture, and its modules for the file select's check
        if (const Disc::File* logo = ra.find("res/Object/TlogoE.arc"); logo && ra.read(*logo, x)) {
            fs::create_directories(m_exeDir / "game" / "rando" / "logo", ec);
            std::ofstream(m_exeDir / "game" / "rando" / "logo" / "TlogoE.arc", std::ios::binary).write(reinterpret_cast<const char*>(x.data()), std::streamsize(x.size()));
        }
        fs::create_directories(m_exeDir / "game" / "rando" / "rels", ec);
        if (const Disc::File* ship = ra.find("rels/d_a_ship.rel"); ship && ra.read(*ship, x)) {
            const Bytes rel = yaz0(x);
            std::ofstream(m_exeDir / "game" / "rando" / "rels" / "d_a_ship.rel", std::ios::binary).write(reinterpret_cast<const char*>(rel.data()), std::streamsize(rel.size()));
        }
    }
    set(Extras, 0.5f, "Link's colours and Swift Sail's textures");
    {
        const fs::path rando = randoDir(m_exeDir);
        std::wstring env;
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* p = block; *p; p += wcslen(p) + 1) env.append(p).push_back(L'\0');
            FreeEnvironmentStringsW(block);
        }
        env.append(L"PYTHONPATH=" + (rando / "site-packages").wstring()).push_back(L'\0');
        env.append(L"RANDO_RUNTIME=" + rando.wstring()).push_back(L'\0');
        env.push_back(L'\0');
        const fs::path iso = fs::exists(m_exeDir / "game" / "GZLE01.iso", ec) ? m_exeDir / "game" / "GZLE01.iso" : m_disc;
        const std::wstring jobs[] = {
            L"\"" + (rando / "link_colors.py").wstring() + L"\" \"" + iso.wstring() + L"\" \"" + (m_exeDir / "assets" / "link_colors").wstring() +
                L"\" --masks \"" + (rando / "wwrando" / "assets" / "link_color_masks").wstring() + L"\"",
            L"\"" + (rando / "swift_sail.py").wstring() + L"\" \"" + iso.wstring() + L"\" \"" + (m_exeDir / "assets" / "swift_sail").wstring() + L"\"",
            // the Bunny Hood's model and icon, made from the disc's wall mask and Link (Boat of Daphnes' bunny_hood.py)
            L"\"" + (rando / "bunny_hood.py").wstring() + L"\" \"" + iso.wstring() + L"\" \"" + (m_exeDir / "assets" / "bunnyhood").wstring() + L"\""};
        int n = 0;
        for (const std::wstring& args : jobs) {
            HANDLE job = makeJob();
            Child c;
            std::string err;
            if (!spawn(job, rando / "python" / "python.exe", args, rando, m_work / ("extras" + std::to_string(n) + ".log"), c, err, env.c_str())) {
                CloseHandle(job);
                log("not made: " + err);
                continue;
            }
            WaitForSingleObject(c.proc, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(c.proc, &code);
            CloseHandle(c.proc), CloseHandle(job);
            if (code != 0) log("extras " + std::to_string(n) + " failed:\n" + tailText(m_work / ("extras" + std::to_string(n) + ".log"), 10));
            set(Extras, 0.5f + 0.5f / 3.f * float(++n), "Link's colours, Swift Sail's textures and the Bunny Hood");
        }
    }
    set(Extras, 1, "");
    return true;
}

}  // namespace setup
