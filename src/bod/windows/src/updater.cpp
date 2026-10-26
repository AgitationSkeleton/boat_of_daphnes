// Updates: see updater.h.
//
// Release tags look like v2026.10.24c (year.month.day plus a letter for the 2nd, 3rd... build of a day). The running
// version comes from BLUEWAKE_TEST_VERSION (tests), else version.txt beside the program (the packaging scripts write it
// and the installer rewrites it), else a build-time BOD_VERSION; a build made from source has none: it is a "development
// build", which can check but does not replace itself.
//
// Flow: check (the API, one small JSON) -> Available -> [Update Now | Install Updates Automatically] -> download to
// update/pkg -> verify the size -> unpack to update/staged (Windows: the app's libarchive; Linux: tar) -> Ready -> when the
// game closes, OnExit swaps the staged files in (a file that exists is first renamed to *.old: a running program cannot be
// overwritten on Windows but can be renamed, and on Linux the running binary is simply replaced) -> the menu's restart
// starts the new version. The *.old files are deleted at the next start.
#include "updater.h"

#include "notifications.h"
#include "platform/bod_platform.h"
#include "ship/cvar.h"
#include "ship_menu.h"
#include <imgui.h>

#define PICOJSON_USE_INT64
#include "picojson.h"

#ifdef _WIN32
#include "mod_archive.h"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace updater {
namespace {

constexpr const char* kApiBase = "https://api.github.com";
constexpr const char* kApiPath = "/repos/AgitationSkeleton/boat_of_daphnes/releases/latest";
#ifdef _WIN32
constexpr const char* kAssetSuffix = "-windows-x64.zip";
constexpr const char* kProgram = "boat-of-daphnes.exe";
#else
constexpr const char* kAssetSuffix = "-linux-x86_64.tar.gz";
constexpr const char* kProgram = "boat-of-daphnes";
#endif
constexpr long long kStartDelayMs = 8000;

enum class Phase { Idle, Checking, UpToDate, Available, Downloading, Extracting, Ready, Installing, Error };

std::mutex g_mutex;
Phase g_phase = Phase::Idle;
std::string g_latest;     // the newest release's tag
std::string g_asset_url;  // this OS's archive
uint64_t g_asset_size = 0;
std::string g_error;      // shown while Phase::Error
std::string g_detail;     // "Unpacking 120 of 1500 files"
uint64_t g_got = 0, g_total = 0;
bool g_install_requested = false;  // Update Now: install when the game closes, then restart
bool g_toasted = false;
bool g_started = false;  // the startup check was begun
bool g_cleanup_started = false;
bool g_have_first_frame = false;
std::atomic<bool> g_applied{false};  // OnExit installed an update
std::chrono::steady_clock::time_point g_first_frame;

std::thread g_worker;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancel{false};

// ---- versions ----------------------------------------------------------------------------------------------------

struct Tag {
    bool ok = false;
    int year = 0, month = 0, day = 0;
    std::string suffix;  // "" < "a" < "b" < ... < "z" < "aa"
};

std::string Trimmed(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1])))
        --e;
    return in.substr(b, e - b);
}

Tag ParseTag(const std::string& text) {
    Tag t;
    std::string s = Trimmed(text);
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
        s.erase(s.begin());
    size_t i = 0;
    auto number = [&](int* out) {
        const size_t start = i;
        long v = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && i - start < 6)
            v = v * 10 + (s[i++] - '0');
        *out = static_cast<int>(v);
        return i > start;
    };
    if (!number(&t.year) || i >= s.size() || s[i++] != '.' || !number(&t.month) || i >= s.size() || s[i++] != '.' ||
        !number(&t.day))
        return t;
    for (; i < s.size(); ++i) {
        if (!std::islower(static_cast<unsigned char>(s[i])))
            return t;
        t.suffix += s[i];
    }
    t.ok = true;
    return t;
}

// <0, 0, >0 like strcmp.
int CompareTags(const Tag& a, const Tag& b) {
    if (a.year != b.year) return a.year < b.year ? -1 : 1;
    if (a.month != b.month) return a.month < b.month ? -1 : 1;
    if (a.day != b.day) return a.day < b.day ? -1 : 1;
    if (a.suffix.size() != b.suffix.size()) return a.suffix.size() < b.suffix.size() ? -1 : 1;
    return a.suffix.compare(b.suffix);
}

fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string ToUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
std::string Generic(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

fs::path Root() { return bodplat::ExeFolder(); }
fs::path UpdateDir() { return Root() / "update"; }

std::string CurrentVersion() {
    if (const char* v = std::getenv("BLUEWAKE_TEST_VERSION"))
        if (*v)
            return Trimmed(v);
    std::ifstream in(Root() / "version.txt", std::ios::binary);
    std::string line;
    std::getline(in, line);
    line = Trimmed(line);
#ifdef BOD_VERSION
    if (line.empty())
        line = BOD_VERSION;
#endif
    return line;
}

bool OptNotify() { return CVarGetInteger("gSettings.Updates.Notify", 1) != 0; }
bool OptAuto() { return CVarGetInteger("gSettings.Updates.AutoInstall", 0) != 0; }

void SetPhase(Phase p) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_phase = p;
}

void Log(const char* what, const std::string& detail = std::string()) {
    std::fprintf(stderr, "[updater] %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
}

void Fail(const std::string& why) {
    Log("failed", why);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_phase = Phase::Error;
    g_error = why;
}

std::string Lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string& s, const char* tail) {
    const std::string t = tail;
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// ---- unpacking ---------------------------------------------------------------------------------------------------

bool SafeRelative(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos)
        return false;
    size_t i = 0;
    while (i <= name.size()) {
        size_t j = name.find_first_of("/\\", i);
        if (j == std::string::npos)
            j = name.size();
        if (name.compare(i, j - i, "..") == 0)
            return false;
        i = j + 1;
    }
    return true;
}

bool Unpack(const fs::path& archive, const fs::path& into, std::string* error) {
    std::error_code ec;
    fs::create_directories(into, ec);
#ifdef _WIN32
    ModArchive* a = mod_archive_open(ToUtf8(archive).c_str());
    if (!a) {
        *error = "the download is not a readable zip";
        return false;
    }
    const unsigned count = mod_archive_count(a);
    bool ok = true;
    for (unsigned i = 0; i < count && ok; ++i) {
        if (g_cancel) {
            *error = "cancelled";
            ok = false;
            break;
        }
        const std::string name = mod_archive_name(a, i);
        if (!SafeRelative(name)) {
            *error = "the archive holds an unsafe path: " + name;
            ok = false;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_detail = "unpacking " + std::to_string(i + 1) + " of " + std::to_string(count) + " files";
        }
        unsigned long long size = 0;
        unsigned char* data = mod_archive_read(a, i, &size);
        if (!data) {
            *error = "could not unpack " + name;
            ok = false;
            break;
        }
        const fs::path dest = into / FromUtf8(name);
        fs::create_directories(dest.parent_path(), ec);
        {
            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            out.close();
            if (!out) {
                *error = "could not write " + name + " (is the drive full?)";
                ok = false;
            }
        }
        std::free(data);
    }
    mod_archive_close(a);
    return ok;
#else
    bodplat::Process p;
    bodplat::Process::Options o;
    o.program = fs::exists("/usr/bin/tar", ec) ? "/usr/bin/tar" : "/bin/tar";
    o.args = {"-xzf", ToUtf8(archive), "-C", ToUtf8(into)};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_detail = "unpacking...";
    }
    if (!p.Start(o)) {
        *error = "tar could not be started";
        return false;
    }
    if (p.Wait() != 0) {
        *error = "tar could not unpack the download";
        return false;
    }
    return true;
#endif
}

// The folder holding the program inside the unpacked archive: the folder itself, or its only subfolder.
fs::path PackageRoot(const fs::path& staged) {
    std::error_code ec;
    if (fs::exists(staged / kProgram, ec))
        return staged;
    for (const auto& e : fs::directory_iterator(staged, ec))
        if (e.is_directory(ec) && fs::exists(e.path() / kProgram, ec))
            return e.path();
    return fs::path();
}

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool CanWriteHere(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path probe = folder / ".write_test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        out << "x";
    }
    const bool ok = fs::exists(probe, ec);
    fs::remove(probe, ec);
    return ok;
}

// ---- downloading and unpacking (worker thread) -------------------------------------------------------------------

// Removes a download and what was unpacked from it (applied.txt, the list of old files to delete, stays).
void ClearStaging(const fs::path& dir) {
    std::error_code ec;
    for (const char* name : {"pkg.part", "pkg", "staged.txt"})
        fs::remove(dir / name, ec);
    fs::remove_all(dir / "staged", ec);
    fs::remove(dir, ec);  // (only if nothing is left)
}

void StageWorker(std::string tag, std::string url, uint64_t expected) {
    const fs::path dir = UpdateDir();
    std::error_code ec;
    auto finish = [&](bool cancelled, const std::string& why) {
        ClearStaging(dir);
        if (cancelled)
            SetPhase(Phase::Available);
        else
            Fail(why);
        g_busy = false;
    };
    ClearStaging(dir);
    if (!CanWriteHere(dir))
        return finish(false, "the game's folder cannot be written to; download the update from the releases page instead");
    const fs::path part = dir / "pkg.part", pkg = dir / "pkg";
    SetPhase(Phase::Downloading);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_got = 0;
        g_total = expected;
        g_detail.clear();
    }
    Log("downloading", url);
    std::string error;
    const bool got = bodplat::HttpDownload(
        url, ToUtf8(part),
        [](uint64_t done, uint64_t total) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_got = done;
            if (total != 0)
                g_total = total;
            return !g_cancel.load();
        },
        &error);
    if (!got)
        return finish(g_cancel.load(), "download failed: " + error);
    const uint64_t size = fs::file_size(part, ec);
    if (ec || (expected != 0 && size != expected))
        return finish(false, "the download is incomplete (" + std::to_string(size) + " of " + std::to_string(expected) + " bytes)");
    fs::rename(part, pkg, ec);
    SetPhase(Phase::Extracting);
    const fs::path staged = dir / "staged";
    if (!Unpack(pkg, staged, &error))
        return finish(g_cancel.load(), error);
    fs::remove(pkg, ec);
    const fs::path pkg_root = PackageRoot(staged);
    if (pkg_root.empty())
        return finish(false, "the download does not hold the game");
    // staged.txt, written last = the unpacked update is complete: the tag, then the folder holding the files
    WriteText(dir / "staged.txt", tag + "\n" + Generic(fs::relative(pkg_root, dir, ec)) + "\n");
    Log("ready to install", tag);
    SetPhase(Phase::Ready);
    g_busy = false;
}

// ---- installing --------------------------------------------------------------------------------------------------

// Swaps the staged files in; the error text, empty when done.
std::string Install(const std::string& tag) {
    const fs::path dir = UpdateDir();
    std::error_code ec;
    std::string staged_tag, rel;
    {
        std::ifstream in(dir / "staged.txt", std::ios::binary);
        std::getline(in, staged_tag);
        std::getline(in, rel);
    }
    staged_tag = Trimmed(staged_tag);
    rel = Trimmed(rel);
    if (staged_tag.empty() || rel.empty())
        return "no complete update is waiting";
    const fs::path src = dir / FromUtf8(rel);
    const fs::path root = Root();

    struct Swap {
        fs::path dest, old;
    };
    std::vector<Swap> done;
    auto rollback = [&]() {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            fs::remove(it->dest, ec);
            if (!it->old.empty())
                fs::rename(it->old, it->dest, ec);
        }
    };
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(src, ec))
        if (e.is_regular_file(ec))
            files.push_back(e.path());
    std::string applied;
    for (const fs::path& file : files) {
        const std::string generic = Generic(fs::relative(file, src, ec));
        const fs::path dest = root / FromUtf8(generic);
        const bool exists = fs::exists(dest, ec);
        if (IsPlayerFile(generic, exists))
            continue;
        fs::create_directories(dest.parent_path(), ec);
        Swap s;
        s.dest = dest;
        if (exists) {
            s.old = dest;
            s.old += ".old";
            fs::remove(s.old, ec);  // (a leftover of an earlier update)
            ec.clear();
            fs::rename(dest, s.old, ec);
            if (ec) {
                const std::string why = ec.message();
                rollback();
                return "could not replace " + generic + " (" + why + "); close other programs using the game's folder";
            }
        }
        fs::rename(file, dest, ec);
        if (ec) {  // another volume, or the like: copy
            ec.clear();
            fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            const std::string why = ec.message();
            if (!s.old.empty())
                fs::rename(s.old, dest, ec);
            rollback();
            return "could not write " + generic + " (" + why + ")";
        }
        done.push_back(s);
        if (!s.old.empty())
            applied += ToUtf8(s.old) + "\n";
    }
    const std::string& t = tag.empty() ? staged_tag : tag;
    WriteText(root / "version.txt", (t[0] == 'v' || t[0] == 'V' ? t.substr(1) : t) + "\n");
    // what the next start deletes
    fs::remove_all(dir / "staged", ec);
    fs::remove(dir / "staged.txt", ec);
    WriteText(dir / "applied.txt", applied);
    Log("installed", t + ", " + std::to_string(done.size()) + " files replaced or added");
    return std::string();
}

// At the start: the *.old files of an earlier install are deleted (the closing old process may still hold them, so it keeps
// trying for a minute), and an unfinished download is removed.
void CleanupWorker() {
    const fs::path dir = UpdateDir();
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return;
    std::vector<fs::path> pending;
    {
        std::ifstream in(dir / "applied.txt", std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            line = Trimmed(line);
            if (!line.empty())
                pending.push_back(FromUtf8(line));
        }
    }
    for (int tries = 0; tries < 60 && !pending.empty() && !g_cancel; ++tries) {
        for (size_t i = 0; i < pending.size();) {
            fs::remove(pending[i], ec);
            if (!fs::exists(pending[i], ec))
                pending.erase(pending.begin() + static_cast<long>(i));
            else
                ++i;
        }
        if (!pending.empty())
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (pending.empty()) {
        fs::remove(dir / "applied.txt", ec);
        fs::remove(dir, ec);  // (only if empty: a download may be under way in it)
    }
}

// A complete download from an earlier run: Ready again, when it is newer than this version.
void AdoptStaged() {
    const fs::path file = UpdateDir() / "staged.txt";
    std::error_code ec;
    if (!fs::exists(file, ec))
        return;
    std::string tag;
    {
        std::ifstream in(file, std::ios::binary);
        std::getline(in, tag);
    }
    tag = Trimmed(tag);
    if (!tag.empty() && IsNewer(tag, CurrentVersion()) && fs::exists(UpdateDir() / "staged", ec)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest = tag;
        g_phase = Phase::Ready;
        Log("an update is waiting", tag);
    } else {
        fs::remove_all(UpdateDir() / "staged", ec);
        fs::remove(file, ec);
    }
}

// ---- the check (worker thread) -----------------------------------------------------------------------------------

void CheckWorker() {
    auto fail = [](const std::string& why) {
        Fail(why);
        g_busy = false;
    };
    const bodplat::HttpResult r = bodplat::HttpRequest(kApiBase, "GET", kApiPath, nullptr, 4u << 20);
    if (!r.ok || r.status == 0)
        return fail("could not reach GitHub (no network?)");
    if (r.status == 403 || r.status == 429)
        return fail("GitHub is limiting requests from this network; try again later");
    if (r.status != 200)
        return fail("GitHub answered " + std::to_string(r.status));
    picojson::value v;
    const std::string body(r.body.begin(), r.body.end());
    const std::string perr = picojson::parse(v, body);
    if (!perr.empty() || !v.is<picojson::object>())
        return fail("GitHub's answer could not be read");
    const auto& o = v.get<picojson::object>();
    const auto tag_it = o.find("tag_name");
    if (tag_it == o.end() || !tag_it->second.is<std::string>())
        return fail("GitHub's answer has no release");
    const std::string tag = tag_it->second.get<std::string>();
    std::string url;
    uint64_t size = 0;
    const auto assets = o.find("assets");
    if (assets != o.end() && assets->second.is<picojson::array>()) {
        for (const picojson::value& a : assets->second.get<picojson::array>()) {
            if (!a.is<picojson::object>())
                continue;
            const auto& ao = a.get<picojson::object>();
            const auto n = ao.find("name"), u = ao.find("browser_download_url"), s = ao.find("size");
            if (n == ao.end() || u == ao.end() || !n->second.is<std::string>() || !u->second.is<std::string>())
                continue;
            if (EndsWith(n->second.get<std::string>(), kAssetSuffix)) {
                url = u->second.get<std::string>();
                size = s != ao.end() && s->second.is<int64_t>() ? static_cast<uint64_t>(s->second.get<int64_t>()) : 0;
            }
        }
    }
    const std::string current = CurrentVersion();
    Log("checked", "latest " + tag + ", running " + (current.empty() ? "(development build)" : current));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest = tag;
        g_asset_url = url;
        g_asset_size = size;
        if (!IsNewer(tag, current))
            g_phase = Phase::UpToDate;
        else if (url.empty()) {
            g_phase = Phase::Error;
            g_error = "version " + tag + " has no download for this system";
        } else
            g_phase = Phase::Available;
    }
    g_busy = false;
}

void JoinFinished() {
    if (g_worker.joinable() && !g_busy)
        g_worker.join();
}

void StartCheck() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_busy || g_phase == Phase::Ready || g_phase == Phase::Installing)
            return;
        g_phase = Phase::Checking;
    }
    JoinFinished();
    g_busy = true;
    g_worker = std::thread(CheckWorker);
}

void StartStage() {
    std::string tag, url;
    uint64_t size;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_busy || g_phase != Phase::Available || g_asset_url.empty())
            return;
        tag = g_latest;
        url = g_asset_url;
        size = g_asset_size;
        g_phase = Phase::Downloading;
    }
    JoinFinished();
    g_cancel = false;
    g_busy = true;
    g_worker = std::thread(StageWorker, tag, url, size);
}

} // namespace

bool IsNewer(const std::string& latest, const std::string& current) {
    const Tag a = ParseTag(latest), b = ParseTag(current);
    return a.ok && b.ok && CompareTags(a, b) > 0;
}

bool IsPlayerFile(const std::string& relative, bool existsAlready) {
    const std::string rel = Lower(relative);
    const size_t slash = rel.find('/');
    const std::string first = rel.substr(0, slash);
    static const char* const folders[] = {"saves", "save", "logs", "models", "cache", "randomizer", "game", "update", "screenshots"};
    for (const char* f : folders)
        if (first == f)
            return true;
    if (first == "mods" && existsAlready)
        return true;
    if (first == "assets" && slash != std::string::npos) {  // (made from the player's disc on the first start)
        const size_t next = rel.find('/', slash + 1);
        const std::string second = rel.substr(slash + 1, next == std::string::npos ? std::string::npos : next - slash - 1);
        if (second == "link_colors" || second == "swift_sail" || second == "bunnyhood")
            return true;
    }
    const size_t last = rel.rfind('/');
    const std::string leaf = rel.substr(last == std::string::npos ? 0 : last + 1);
    return leaf.rfind("tww_game", 0) == 0 || leaf == "boat-of-daphnes.json" || leaf == "controller_ports.dat" ||
           leaf == "keyboard_bindings.dat" || leaf == "launch.pending" || EndsWith(leaf, ".controller") ||
           EndsWith(leaf, ".raw") || EndsWith(leaf, ".slots.json") || EndsWith(leaf, ".old");
}

void Frame(void (*restart)()) {
    const auto now = std::chrono::steady_clock::now();
    if (!g_have_first_frame) {
        g_have_first_frame = true;
        g_first_frame = now;
        if (std::getenv("BLUEWAKE_TEST_UPDATER")) {  // the pure logic, printed once (tests)
            const char* cases[][2] = {{"v2026.10.24c", "2026.10.24b"}, {"v2026.10.24c", "v2026.10.24c"}, {"v2026.10.25", "v2026.10.24c"},
                                      {"v2026.10.24", "v2026.10.24a"}, {"v2026.10.24aa", "v2026.10.24z"}, {"v2026.11.1", "v2026.10.31z"},
                                      {"v2027.1.1", "v2026.12.31"}, {"nonsense", "v2026.10.24"}, {"v2026.10.24a", ""}};
            for (auto& c : cases)
                std::fprintf(stderr, "[updater] IsNewer(%s, %s) = %d\n", c[0], c[1], IsNewer(c[0], c[1]) ? 1 : 0);
            const char* paths[] = {"saves/MemoryCardA.raw", "boat-of-daphnes.json", "tww_game.dll", "tww_game_rando.txt", "logs/x.txt",
                                   "mods/readme.txt", "assets/link_colors/a.png", "boat-of-daphnes.exe", "rando/patch_game.py",
                                   "assets/fonts/a.ttf"};
            for (const char* p : paths)
                std::fprintf(stderr, "[updater] IsPlayerFile(%s) = %d\n", p, IsPlayerFile(p, true) ? 1 : 0);
        }
    }
    if (!g_cleanup_started) {
        g_cleanup_started = true;
        AdoptStaged();
        std::thread(CleanupWorker).detach();
    }
    // The check a few seconds after the start. Not for a development build, and not at all when the player wants neither the
    // notice nor the automatic update.
    if (!g_started && !CurrentVersion().empty() && (OptNotify() || OptAuto()) &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_first_frame).count() >= kStartDelayMs) {
        g_started = true;
        StartCheck();
    }
    Phase phase;
    std::string latest;
    bool install;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        phase = g_phase;
        latest = g_latest;
        install = g_install_requested;
    }
    if ((phase == Phase::Available || phase == Phase::Ready) && !g_toasted && g_started) {
        g_toasted = true;  // once per start
        if (OptNotify()) {
            notifications::Options o;
            o.prefix = "Update available:";
            o.prefixColor = ImVec4(0.4f, 0.9f, 0.5f, 1.0f);
            o.message = latest + " - Settings > Updates";
            Log("toast", o.message);
            notifications::Emit(std::move(o));
        }
    }
    if (phase == Phase::Available && std::getenv("BLUEWAKE_TEST_UPDATE_NOW")) {  // tests: the Update Now button, pressed
        std::lock_guard<std::mutex> lock(g_mutex);
        g_install_requested = true;
    }
    // Install Updates Automatically: fetched now in the background, installed when the game closes. Update Now: the same,
    // then the game closes itself.
    if (phase == Phase::Available && (install || (OptAuto() && g_started)))
        StartStage();
    if (install && phase == Phase::Ready && restart) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_install_requested = false;
            g_phase = Phase::Installing;
        }
        Log("restarting to install", latest);
        restart();  // saves the settings and closes the game; OnExit swaps the files in and the new version starts
    }
    JoinFinished();
}

void OnExit() {
    if (g_applied)
        return;
    g_cancel = true;
    if (g_worker.joinable())
        g_worker.join();
    std::string tag;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool go = g_phase == Phase::Installing || (g_phase == Phase::Ready && OptAuto());
        if (!go)
            return;  // (a download cut short by quitting leaves nothing behind: StageWorker removes it)
        tag = g_latest;
    }
    g_applied = true;
    const std::string error = Install(tag);
    if (!error.empty()) {
        Log("install failed", error);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_phase = Phase::Error;
        g_error = error;
    }
}

void BuildMenu(shipmenu::Menu& m) {
    using shipmenu::WidgetInfo;
    using shipmenu::WidgetType;
    const char* H = "Settings";
    const char* S = "Updates";
    m.AddSidebar(H, S, 1);
    m.AddWidget(H, S, 0, "Updates", WidgetType::SeparatorText);
    m.AddWidget(H, S, 0, "Notify About Updates", WidgetType::CVarCheckbox)
        .CVar("gSettings.Updates.Notify")
        .DefaultValue(true)
        .Tooltip("A few seconds after the game starts it asks GitHub whether a newer release exists (one small request, nothing "
                 "about you is sent) and shows a notice if there is one. Off: no check is made unless you press Check for Updates "
                 "(or Install Updates Automatically is on).");
    m.AddWidget(H, S, 0, "Install Updates Automatically", WidgetType::CVarCheckbox)
        .CVar("gSettings.Updates.AutoInstall")
        .DefaultValue(false)
        .Tooltip("A newer release is downloaded in the background while you play and installed when you next close the game "
                 "(never in the middle of a session). Your saves, settings and mods are kept.");
    m.AddWidget(H, S, 0, "Status", WidgetType::Custom).HideInSearch().CustomFunction([](WidgetInfo&) {
        Phase phase;
        std::string latest, error, detail;
        uint64_t got, total;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            phase = g_phase;
            latest = g_latest;
            error = g_error;
            detail = g_detail;
            got = g_got;
            total = g_total;
        }
        const std::string current = CurrentVersion();
        ImGui::Text("Installed version: %s", current.empty() ? "development build" : current.c_str());
        ImGui::Text("Latest version: %s", latest.empty() ? "not checked yet" : latest.c_str());
        switch (phase) {
        case Phase::Idle:
            ImGui::TextWrapped("Status: not checked yet");
            break;
        case Phase::Checking:
            ImGui::TextWrapped("Status: checking for updates...");
            break;
        case Phase::UpToDate:
            ImGui::TextWrapped("Status: you have the newest version.");
            break;
        case Phase::Available:
            ImGui::TextWrapped("Status: version %s is available.", latest.c_str());
            break;
        case Phase::Downloading:
            ImGui::TextWrapped("Status: downloading %.1f of %.1f MB", got / 1048576.0, total / 1048576.0);
            if (total != 0)
                ImGui::ProgressBar(static_cast<float>(static_cast<double>(got) / static_cast<double>(total)), ImVec2(-1, 0));
            break;
        case Phase::Extracting:
            ImGui::TextWrapped("Status: %s", detail.empty() ? "unpacking..." : detail.c_str());
            break;
        case Phase::Ready:
            ImGui::TextWrapped("Status: version %s is downloaded; it %s.", latest.c_str(),
                               OptAuto() ? "installs when you close the game" : "installs when you press Update Now");
            break;
        case Phase::Installing:
            ImGui::TextWrapped("Status: installing %s and restarting...", latest.c_str());
            break;
        case Phase::Error:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            ImGui::TextWrapped("Error: %s", error.c_str());
            ImGui::PopStyleColor();
            break;
        }
        const float w = ImGui::GetContentRegionAvail().x;
        const bool working = phase == Phase::Checking || phase == Phase::Downloading || phase == Phase::Extracting ||
                             phase == Phase::Installing;
        ImGui::BeginDisabled(working);
        if (ImGui::Button("Check for Updates", ImVec2(w, 0)))
            StartCheck();
        ImGui::EndDisabled();
        const bool newer = IsNewer(latest, current);
        const bool can = newer && (phase == Phase::Available || phase == Phase::Ready);
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Update Now", ImVec2(w, 0))) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_install_requested = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", current.empty() ? "A development build does not replace itself."
                                                    : "Downloads the newest version, installs it over this one (your saves, "
                                                      "settings and mods stay), closes the game and starts it again.");
    });
    m.AddWidget(H, S, 0, "Your saves, settings, logs, mods and the game built from your disc are never touched by an update.",
                WidgetType::Text);
}

} // namespace updater
