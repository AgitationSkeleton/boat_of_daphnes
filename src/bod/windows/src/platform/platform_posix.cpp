// The Linux side of the platform layer (bod_platform.h). Sockets, TLS, HTTP and WebSocket are in net_posix.cpp.
#include "bod_platform.h"

#include <SDL3/SDL.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

extern char** environ;

namespace bodplat {
namespace {
namespace fs = std::filesystem;

std::vector<std::string> g_environment;  // as the game was started, for Relaunch
std::vector<std::string> g_arguments;

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::string read_text(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// file:///a/b%20c for the path (the characters a URI keeps are left alone).
std::string file_uri(const fs::path& path) {
    std::error_code ec;
    const std::string absolute = fs::absolute(path, ec).string();
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : absolute) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool is_executable_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
}

// What the clipboard hands out for each type it offers: one set of strings kept until the next copy.
struct ClipboardData {
    std::string uri_list, gnome, plain;
};
ClipboardData* g_clipboard = nullptr;

const void* SDLCALL clipboard_data(void* user, const char* mime, size_t* size) {
    const ClipboardData* data = static_cast<ClipboardData*>(user);
    const std::string* text = nullptr;
    if (std::strcmp(mime, "text/uri-list") == 0)
        text = &data->uri_list;
    else if (std::strcmp(mime, "x-special/gnome-copied-files") == 0)
        text = &data->gnome;
    else
        text = &data->plain;
    *size = text->size();
    return text->data();
}

void SDLCALL clipboard_cleanup(void* user) {
    delete static_cast<ClipboardData*>(user);
    if (g_clipboard == user)
        g_clipboard = nullptr;
}

}  // namespace

fs::path ExeFolder() {
    char path[4096];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0)
        return fs::current_path();
    path[n] = '\0';
    return fs::path(path).parent_path();
}

fs::path DataFolder() {
    if (const char* data = std::getenv("BLUEWAKE_DATA_DIR"); data != nullptr && data[0] != '\0')
        return fs::path(data);
    return ExeFolder();
}

uint64_t TickMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

unsigned long CurrentPid() { return static_cast<unsigned long>(::getpid()); }

void SetEnv(const char* name, const std::string& value) { ::setenv(name, value.c_str(), 1); }

void OpenPath(const fs::path& path) { SDL_OpenURL(file_uri(path).c_str()); }

void RevealFile(const fs::path& file) { SDL_OpenURL(file_uri(file.parent_path()).c_str()); }

// The file as the file manager's Copy puts it: a list of addresses, which a chat window or a folder takes.
void CopyFileToClipboard(const fs::path& file) {
    auto* data = new ClipboardData;
    const std::string uri = file_uri(file);
    data->uri_list = uri + "\r\n";
    data->gnome = "copy\n" + uri;
    data->plain = fs::absolute(file).string();
    static const char* types[] = {"text/uri-list", "x-special/gnome-copied-files", "text/plain;charset=utf-8"};
    g_clipboard = data;
    if (!SDL_SetClipboardData(clipboard_data, clipboard_cleanup, data, types, 3))
        SDL_SetClipboardText(data->plain.c_str());
}

std::string MachineId() {
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        const std::string id = trimmed(read_text(path));
        if (!id.empty())
            return id;
    }
    return {};
}

uint64_t TotalMemoryBytes() {
    const long pages = ::sysconf(_SC_PHYS_PAGES), size = ::sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(size) : 0;
}

// Threads are the CPUs the kernel lists; a core is each distinct (package, core id) among them.
CpuCounts Cpu() {
    CpuCounts counts;
    std::set<std::pair<std::string, std::string>> cores;
    std::error_code ec;
    for (fs::directory_iterator it("/sys/devices/system/cpu", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 || !std::isdigit(static_cast<unsigned char>(name[3])))
            continue;
        ++counts.threads;
        cores.emplace(trimmed(read_text((it->path() / "topology" / "physical_package_id").c_str())),
                      trimmed(read_text((it->path() / "topology" / "core_id").c_str())));
    }
    counts.cores = static_cast<int>(cores.size());
    if (counts.threads == 0)
        counts.threads = counts.cores = static_cast<int>(std::thread::hardware_concurrency());
    return counts;
}

// The display adapters the kernel lists (vendor and device ids; the names are the drivers' to give).
std::string GpuNames() {
    std::string names;
    std::error_code ec;
    for (fs::directory_iterator it("/sys/class/drm", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.compare(0, 4, "card") != 0 || name.find('-') != std::string::npos)
            continue;
        const std::string vendor = trimmed(read_text((it->path() / "device" / "vendor").c_str()));
        const std::string device = trimmed(read_text((it->path() / "device" / "device").c_str()));
        if (vendor.empty())
            continue;
        const char* maker = vendor == "0x10de" ? "NVIDIA" : vendor == "0x1002" ? "AMD" : vendor == "0x8086" ? "Intel" : vendor.c_str();
        names += (names.empty() ? "" : " + ") + std::string(maker) + " " + device;
    }
    return names;
}

bool PrimaryWorkArea(int*, int*) { return false; }

// The adapter shows in /sys/bus/usb/devices when it is plugged in; opening it (hidraw) needs a udev rule, which the
// reader's own status reports (gc_adapter.c), so there is no "driver needed" here.
GcAdapterState GcAdapterDriverState() {
    std::error_code ec;
    for (fs::directory_iterator it("/sys/bus/usb/devices", ec), end; !ec && it != end; it.increment(ec))
        if (trimmed(read_text((it->path() / "idVendor").c_str())) == "057e" &&
            trimmed(read_text((it->path() / "idProduct").c_str())) == "0337")
            return GcAdapterState::Ready;
    return GcAdapterState::NotFound;
}

std::string SystemUiFontPath() {
    for (const char* path : {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"}) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec))
            return path;
    }
    return {};
}

fs::path PythonExe(const fs::path& runtime) {
    const fs::path bundled = runtime / "python" / "bin" / "python3";
    if (is_executable_file(bundled))
        return bundled;
    if (const char* path = std::getenv("PATH")) {
        std::stringstream dirs(path);
        std::string dir;
        while (std::getline(dirs, dir, ':'))
            if (!dir.empty() && is_executable_file(fs::path(dir) / "python3"))
                return fs::path(dir) / "python3";
    }
    return bundled;
}

void CaptureEnvironment() {
    g_environment.clear();
    for (char** e = environ; e != nullptr && *e != nullptr; ++e)
        g_environment.emplace_back(*e);
    g_arguments.clear();
    const std::string cmdline = read_text("/proc/self/cmdline");
    for (size_t at = 0; at < cmdline.size();) {
        const size_t end = cmdline.find('\0', at);
        g_arguments.push_back(cmdline.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
}

// Start the game again, with the command line and environment it began with, so the settings file decides what the
// new session is.
bool Relaunch() {
    const fs::path exe = ExeFolder() / "boat-of-daphnes";
    std::vector<char*> argv, envp;
    for (std::string& a : g_arguments)
        argv.push_back(a.data());
    if (argv.empty())
        argv.push_back(const_cast<char*>("boat-of-daphnes"));
    argv.push_back(nullptr);
    for (std::string& e : g_environment)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, exe.c_str(), nullptr, &attr, argv.data(), g_environment.empty() ? environ : envp.data());
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        std::fprintf(stderr, "[linux] restart failed (error %d)\n", rc);
        return false;
    }
    std::fprintf(stderr, "[linux] restarting with the new settings\n");
    return true;
}

// --- child processes ---------------------------------------------------------------------------------------------

struct Process::Impl {
    std::atomic<pid_t> pid{0};
    int in = -1;   // our end of the child's standard input
    int out = -1;  // our end of its standard output
    bool reaped = false;
    int status = -1;
};

Process::Process() : mImpl(new Impl) {}

Process::~Process() {
    CloseStdin();
    if (mImpl->pid != 0 && !mImpl->reaped) {  // no zombie left behind
        Kill();
        Wait();
    }
    if (mImpl->out >= 0)
        ::close(mImpl->out);
    delete mImpl;
}

bool Process::Started() const { return mImpl->pid != 0; }

unsigned long Process::Pid() const { return static_cast<unsigned long>(mImpl->pid.load()); }

bool Process::Start(const Options& options) {
    for (const auto& [name, value] : options.env)
        ::setenv(name.c_str(), value.c_str(), 1);
    std::vector<std::string> arguments{options.program.string()};
    arguments.insert(arguments.end(), options.args.begin(), options.args.end());
    std::vector<char*> argv;
    for (std::string& a : arguments)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
    if (options.pipes && (::pipe2(to_child, O_CLOEXEC) != 0 || ::pipe2(from_child, O_CLOEXEC) != 0))
        return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (options.pipes) {
        posix_spawn_file_actions_adddup2(&actions, to_child[0], 0);
        posix_spawn_file_actions_adddup2(&actions, from_child[1], 1);
    } else {
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        if (!options.stdoutFile.empty())
            posix_spawn_file_actions_addopen(&actions, 1, options.stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        else
            posix_spawn_file_actions_adddup2(&actions, 2, 1);  // this process's standard error
    }
    if (!options.stderrFile.empty())
        posix_spawn_file_actions_addopen(&actions, 2, options.stderrFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, options.program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (to_child[0] >= 0)
        ::close(to_child[0]);
    if (from_child[1] >= 0)
        ::close(from_child[1]);
    if (rc != 0) {
        if (to_child[1] >= 0)
            ::close(to_child[1]);
        if (from_child[0] >= 0)
            ::close(from_child[0]);
        return false;
    }
    mImpl->in = to_child[1];
    mImpl->out = from_child[0];
    mImpl->pid = pid;
    return true;
}

bool Process::WriteStdin(const void* data, size_t size) {
    const char* at = static_cast<const char*>(data);
    while (mImpl->in >= 0 && size > 0) {
        const ssize_t n = ::write(mImpl->in, at, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        at += n;
        size -= static_cast<size_t>(n);
    }
    return size == 0 && mImpl->in >= 0;
}

long Process::ReadStdout(void* data, size_t capacity) {
    for (;;) {
        if (mImpl->out < 0)
            return -1;
        const ssize_t n = ::read(mImpl->out, data, capacity);
        if (n < 0 && errno == EINTR)
            continue;
        return static_cast<long>(n);
    }
}

void Process::CloseStdin() {
    if (mImpl->in >= 0) {
        ::close(mImpl->in);
        mImpl->in = -1;
    }
}

int Process::Wait() {
    const pid_t pid = mImpl->pid;
    if (pid == 0)
        return -1;
    if (!mImpl->reaped) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        mImpl->reaped = true;
        mImpl->status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return mImpl->status;
}

void Process::Kill() {
    const pid_t pid = mImpl->pid;
    if (pid != 0 && !mImpl->reaped)
        ::kill(pid, SIGKILL);
}

bool UsbDevicePresent(unsigned short, unsigned short) { return true; }

}  // namespace bodplat
