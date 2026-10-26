// The app's platform layer: what the shared app code (settings menu, Anchor, trackers, randomizer flow) needs from the
// operating system, in one place. platform_win32.cpp is the Windows code those files used to hold, moved; platform_posix.cpp
// (with net_posix.cpp for sockets, TLS, HTTP and WebSocket) is the Linux one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// bw_posix_compat.h (Windows builds force-include it) defines rename(), which <filesystem> declares.
#ifdef rename
#undef rename
#endif
#include <filesystem>

#ifdef _WIN32
#define BOD_SEP "\\"
#else
#define BOD_SEP "/"
#endif

namespace bodplat {

// The folder the executable is in (no trailing separator).
std::filesystem::path ExeFolder();
// Where the player's files live: the executable's folder on Windows. On Linux BLUEWAKE_DATA_DIR (linux_entry.c sets it:
// beside the executable when that can be written to, else the player's data folder, e.g. from an AppImage).
std::filesystem::path DataFolder();
// Milliseconds on a monotonic clock (GetTickCount64 on Windows).
uint64_t TickMs();
unsigned long CurrentPid();

// Sets a variable in this process's environment (children inherit it).
void SetEnv(const char* name, const std::string& value);

// The default program opens the file, folder or address.
void OpenPath(const std::filesystem::path& path);
// The file shown in the file manager.
void RevealFile(const std::filesystem::path& file);
// The file on the clipboard the way the file manager's Copy puts it, so it pastes into a chat or a folder.
void CopyFileToClipboard(const std::filesystem::path& file);

// A string that stays the same on this PC (the Windows MachineGuid, /etc/machine-id); "" when there is none.
std::string MachineId();
// Physical memory in bytes; 0 when unknown.
uint64_t TotalMemoryBytes();
struct CpuCounts {
    int cores = 0, threads = 0;
};
CpuCounts Cpu();
// The graphics adapters' names, " + " apart; "" when the system does not say.
std::string GpuNames();

// The primary screen's work area (the screen less the task bar) in pixels, before the game's window exists; false when it
// cannot be read yet (Linux: SDL's video is not up).
bool PrimaryWorkArea(int* width, int* height);

// The GameCube adapter (USB 057E:0337) as the system sees it. SDL reads it directly only once WinUSB drives it on Windows, so
// "DriverNeeded" is a plugged-in adapter under another driver; on Linux it is Ready when the kernel lists it.
enum class GcAdapterState { NotFound, DriverNeeded, Ready };
GcAdapterState GcAdapterDriverState();

// A TrueType font file for the menu's text (Segoe UI on Windows); "" for none, the built-in font then.
std::string SystemUiFontPath();

// The Python the randomizer's runtime folder (rando/) carries: python/python.exe on Windows; on Linux python/bin/python3
// there, else python3 from the PATH. May not exist.
std::filesystem::path PythonExe(const std::filesystem::path& runtime);

// The environment and command line the app started with, kept for Relaunch (call first thing in main).
void CaptureEnvironment();
// Starts the app again with them; true when the new process started.
bool Relaunch();

// SHA-256 of the bytes.
bool Sha256(const uint8_t* data, size_t size, uint8_t out[32]);

// A child process, optionally with its standard input and output piped to this one.
class Process {
public:
    struct Options {
        std::filesystem::path program;
        std::vector<std::string> args;                       // UTF-8, after the program
        std::vector<std::pair<std::string, std::string>> env;  // set in this process first; the child inherits them
        std::filesystem::path stdoutFile;                    // the child's standard output goes here (not with pipes)
        std::filesystem::path stderrFile;                    // its standard error; empty: this process's standard error
        bool pipes = false;                                  // WriteStdin and ReadStdout talk to it
    };
    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool Start(const Options& options);
    bool Started() const;
    unsigned long Pid() const;
    // Pipes only. ReadStdout blocks and returns the bytes read, 0 or less once the child's output has ended.
    bool WriteStdin(const void* data, size_t size);
    long ReadStdout(void* data, size_t capacity);
    void CloseStdin();
    // Waits for the child; its exit code (-1 when it cannot be known).
    int Wait();
    // Ends the child; safe from another thread (it unblocks a ReadStdout).
    void Kill();

private:
    struct Impl;
    Impl* mImpl;
};

// Pins the certificate of host (a name, no port): connections to it over TLS are accepted only when the SHA-256 of the
// server certificate's SubjectPublicKeyInfo (64 hex digits) equals spki_sha256_hex, and then CA, name and expiry checks
// are skipped (the pin is the trust anchor). Hosts without a pin are validated normally. An empty pin removes it.
void PinCertificate(const std::string& host, const std::string& spki_sha256_hex);

// One HTTP request to base ("https://host" or "http://host:port") + path. The answer is read whole, up to max_response bytes.
struct HttpResult {
    bool ok = false;
    int status = 0;
    std::vector<uint8_t> body;
};
HttpResult HttpRequest(const std::string& base, const char* method, const std::string& path,
                       const std::vector<uint8_t>* body, size_t max_response);
// A GET of url (redirects followed) streamed to the file at path, with progress (got, total; total 0 when the server does
// not say). progress returning false stops it. False, the file removed and *error set, when it did not complete.
bool HttpDownload(const std::string& url, const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress,
                  std::string* error);

// Whether a USB device with this vendor and product id is plugged in now (Windows; elsewhere always true).
bool UsbDevicePresent(unsigned short vid, unsigned short pid);

// A WebSocket client connection (text messages). Receive blocks, and Close from another thread ends it.
class WebSocket {
public:
    enum class Result { Text, Binary, Closed, Error };
    WebSocket();
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // Connects, upgrades GET path to a WebSocket. On failure: false, what step failed and the system's error code.
    bool Connect(const std::string& host, unsigned short port, bool secure, const std::string& path, std::string* what,
                 unsigned long* code);
    // The next whole message (a binary one comes back empty).
    Result Receive(std::string* message);
    bool SendText(const std::string& text);
    // Sends the close frame without waiting for the server's.
    void Shutdown();
    // Lets go of the connection; a Receive in progress on another thread returns.
    void Close();

private:
    struct Impl;
    Impl* mImpl;
};

}  // namespace bodplat
