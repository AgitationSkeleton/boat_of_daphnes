// The Windows side of the platform layer (bod_platform.h): the Win32 code the app's files held, moved here unchanged.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include "bod_platform.h"
#include "pin_registry.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace bodplat {
namespace {
namespace fs = std::filesystem;

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<wchar_t> g_environment;  // as the game was started, for Relaunch

std::string narrow(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// --- Certificate pins: a pinned host is trusted by the SHA-256 of its certificate's SubjectPublicKeyInfo alone ----------
// Before the request is sent WinHTTP's own certificate checks are loosened for that host only (unknown CA, name, dates);
// once the handshake is done (WinHttpSendRequest returned) the server's key is compared with the pin, and nothing is read
// from the server unless it matches.

// True when host is pinned (*pin gets the pin, and req accepts a certificate the system does not know).
bool PreparePin(HINTERNET req, const std::string& host, std::string* pin) {
    if (!pins::Find(host, pin))
        return false;
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
    return true;
}

// Whether the key of the server certificate of req (its handshake is done) hashes to pin.
bool PinMatches(HINTERNET req, const std::string& pin) {
    PCCERT_CONTEXT cert = nullptr;
    DWORD size = sizeof cert;
    if (!WinHttpQueryOption(req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) || cert == nullptr)
        return false;
    bool match = false;
    BYTE* der = nullptr;
    DWORD der_size = 0;
    if (CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &cert->pCertInfo->SubjectPublicKeyInfo, CRYPT_ENCODE_ALLOC_FLAG,
                            nullptr, &der, &der_size)) {
        UCHAR digest[32];
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, der, der_size, digest, sizeof digest) == 0)
            match = pins::ConstantTimeEqual(pins::ToHex(digest, sizeof digest), pin);
        LocalFree(der);
    }
    CertFreeCertificateContext(cert);
    return match;
}

// For a WebSocket to a pinned host: a first, harmless request (HEAD /health) on the same connection handle, so the key is
// checked before the upgrade request, which names the room, goes out. 0 = ok, 1 = no answer, 2 = not the pinned key.
int PinPreflight(HINTERNET connection, const std::string& pin) {
    HINTERNET req = WinHttpOpenRequest(connection, L"HEAD", L"/health", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req)
        return 1;
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
    int result = 1;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (!PinMatches(req, pin))
            result = 2;
        else if (WinHttpReceiveResponse(req, nullptr))
            result = 0;
    }
    WinHttpCloseHandle(req);
    return result;
}

// Writes all of body to req (after a WinHttpSendRequest that announced its length).
bool WriteBody(HINTERNET req, const std::vector<uint8_t>& body) {
    size_t at = 0;
    while (at < body.size()) {
        DWORD wrote = 0;
        const DWORD n = static_cast<DWORD>(std::min<size_t>(body.size() - at, 1 << 20));
        if (!WinHttpWriteData(req, body.data() + at, n, &wrote) || wrote == 0)
            return false;
        at += wrote;
    }
    return true;
}
}  // namespace

void PinCertificate(const std::string& host, const std::string& spki_sha256_hex) { pins::Set(host, spki_sha256_hex); }

fs::path ExeFolder() {
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return fs::path(std::wstring(path, n)).parent_path();
}

fs::path DataFolder() { return ExeFolder(); }

uint64_t TickMs() { return GetTickCount64(); }

unsigned long CurrentPid() { return GetCurrentProcessId(); }

void SetEnv(const char* name, const std::string& value) { _putenv_s(name, value.c_str()); }

void OpenPath(const fs::path& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void RevealFile(const fs::path& file) {
    const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// The file on the clipboard the way Explorer's Copy puts it (CF_HDROP), so it pastes into a chat or a folder.
void CopyFileToClipboard(const fs::path& file) {
    const std::wstring w = fs::absolute(file).wstring();
    const SIZE_T bytes = sizeof(DROPFILES) + (w.size() + 2) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GHND, bytes);
    if (mem == nullptr)
        return;
    auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), w.c_str(), w.size() * sizeof(wchar_t));
    GlobalUnlock(mem);
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        if (SetClipboardData(CF_HDROP, mem) == nullptr)
            GlobalFree(mem);
        CloseClipboard();
    } else {
        GlobalFree(mem);
    }
}

std::string MachineId() {
    char guid[64] = {0};
    DWORD size = sizeof guid;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                 nullptr, guid, &size);
    return guid;
}

uint64_t TotalMemoryBytes() {
    MEMORYSTATUSEX mem{sizeof mem};
    return GlobalMemoryStatusEx(&mem) ? mem.ullTotalPhys : 0;
}

CpuCounts Cpu() {
    CpuCounts counts;
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!info.empty() && GetLogicalProcessorInformation(info.data(), &bytes))
        for (const auto& i : info)
            if (i.Relationship == RelationProcessorCore) {
                ++counts.cores;
                for (ULONG_PTR m = i.ProcessorMask; m != 0; m &= m - 1)
                    ++counts.threads;
            }
    return counts;
}

std::string GpuNames() {
    std::string names;
    DISPLAY_DEVICEA dev{sizeof dev};
    std::vector<std::string> seen;
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dev, 0); ++i, dev.cb = sizeof dev)
        if (std::find(seen.begin(), seen.end(), dev.DeviceString) == seen.end()) {
            seen.emplace_back(dev.DeviceString);
            names += (names.empty() ? "" : " + ") + seen.back();
        }
    return names;
}

bool PrimaryWorkArea(int* width, int* height) {
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info))
        return false;
    *width = info.rcWork.right - info.rcWork.left;
    *height = info.rcWork.bottom - info.rcWork.top;
    return true;
}

GcAdapterState GcAdapterDriverState() {
    GcAdapterState state = GcAdapterState::NotFound;
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE)
        return state;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof info;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &info); ++i) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids),
                                               sizeof ids - sizeof(wchar_t), nullptr))
            continue;
        bool adapter = false;
        for (const wchar_t* id = ids; *id != L'\0'; id += wcslen(id) + 1)
            adapter = adapter || wcsstr(id, L"VID_057E&PID_0337") != nullptr;
        if (!adapter)
            continue;
        wchar_t service[128] = {};
        SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_SERVICE, nullptr, reinterpret_cast<PBYTE>(service),
                                          sizeof service - sizeof(wchar_t), nullptr);
        state = _wcsicmp(service, L"WinUSB") == 0 ? GcAdapterState::Ready : GcAdapterState::DriverNeeded;
        if (state == GcAdapterState::Ready)
            break;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return state;
}

std::string SystemUiFontPath() {
    char fonts[MAX_PATH];
    const UINT length = GetWindowsDirectoryA(fonts, MAX_PATH);
    if (length > 0 && length < MAX_PATH - 32) {
        const std::string path = std::string(fonts) + "\Fonts\segoeui.ttf";
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
    }
    return {};
}

fs::path PythonExe(const fs::path& runtime) { return runtime / "python" / "python.exe"; }

void CaptureEnvironment() {
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr)
        return;
    wchar_t* end = block;
    while (*end != L'\0')
        end += wcslen(end) + 1;
    g_environment.assign(block, end + 1);
    FreeEnvironmentStringsW(block);
}

// Start the game again, with the command line and environment it began with, so the settings file decides what the
// new session is.
bool Relaunch() {
    wchar_t exe[MAX_PATH * 2];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH * 2) == 0)
        return false;
    std::vector<wchar_t> command(GetCommandLineW(), GetCommandLineW() + wcslen(GetCommandLineW()) + 1);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        g_environment.empty() ? nullptr : g_environment.data(), nullptr, &startup, &process)) {
        std::fprintf(stderr, "[windows] restart failed (error %lu)\n", GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::fprintf(stderr, "[windows] restarting with the new settings\n");
    return true;
}

bool Sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data), static_cast<ULONG>(size), out, 32) >= 0;
}

// --- child processes ---------------------------------------------------------------------------------------------

struct Process::Impl {
    std::atomic<HANDLE> process{nullptr};
    HANDLE in = nullptr;   // our end of the child's standard input
    HANDLE out = nullptr;  // our end of its standard output
    DWORD pid = 0;
};

Process::Process() : mImpl(new Impl) {}

Process::~Process() {
    CloseStdin();
    if (mImpl->out != nullptr)
        CloseHandle(mImpl->out);
    HANDLE p = mImpl->process.exchange(nullptr);
    if (p != nullptr)
        CloseHandle(p);
    delete mImpl;
}

bool Process::Started() const { return mImpl->process != nullptr; }

unsigned long Process::Pid() const { return mImpl->pid; }

bool Process::Start(const Options& options) {
    std::wstring cmd = L"\"" + options.program.wstring() + L"\"";
    for (const std::string& a : options.args)
        cmd += L" \"" + widen(a) + L"\"";
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;  // the standard output file, or the standard error file with pipes
    if (options.pipes) {
        if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0))
            return false;
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        if (!options.stderrFile.empty())
            file = CreateFileW(options.stderrFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = file != INVALID_HANDLE_VALUE ? file : GetStdHandle(STD_ERROR_HANDLE);
    } else {
        if (!options.stdoutFile.empty()) {
            file = CreateFileW(options.stdoutFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
        }
        si.hStdOutput = file != INVALID_HANDLE_VALUE ? file : GetStdHandle(STD_ERROR_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    for (const auto& [name, value] : options.env)
        SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (inR != nullptr)
        CloseHandle(inR);
    if (outW != nullptr)
        CloseHandle(outW);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (!started) {
        if (inW != nullptr)
            CloseHandle(inW);
        if (outR != nullptr)
            CloseHandle(outR);
        return false;
    }
    CloseHandle(pi.hThread);
    mImpl->in = inW;
    mImpl->out = outR;
    mImpl->pid = pi.dwProcessId;
    mImpl->process = pi.hProcess;
    return true;
}

bool Process::WriteStdin(const void* data, size_t size) {
    DWORD wrote = 0;
    return mImpl->in != nullptr && WriteFile(mImpl->in, data, static_cast<DWORD>(size), &wrote, nullptr);
}

long Process::ReadStdout(void* data, size_t capacity) {
    DWORD got = 0;
    if (mImpl->out == nullptr || !ReadFile(mImpl->out, data, static_cast<DWORD>(capacity), &got, nullptr))
        return -1;
    return static_cast<long>(got);
}

void Process::CloseStdin() {
    if (mImpl->in != nullptr) {
        CloseHandle(mImpl->in);
        mImpl->in = nullptr;
    }
}

int Process::Wait() {
    HANDLE p = mImpl->process;
    if (p == nullptr)
        return -1;
    WaitForSingleObject(p, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p, &code);
    return static_cast<int>(code);
}

void Process::Kill() {
    HANDLE p = mImpl->process;
    if (p != nullptr)
        TerminateProcess(p, 0);
}

// --- HTTP -------------------------------------------------------------------------------------------------------

bool HttpDownload(const std::string& url_text, const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress,
                  std::string* error) {
    auto failed = [&](const char* why) {
        if (error)
            *error = why;
        DeleteFileW(widen(path).c_str());
        return false;
    };
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    wchar_t host[256];
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.dwSchemeLength = static_cast<DWORD>(-1);
    u.dwUrlPathLength = static_cast<DWORD>(-1);
    u.dwExtraInfoLength = static_cast<DWORD>(-1);
    const std::wstring url = widen(url_text);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &u))
        return failed("not a URL");
    HINTERNET session = WinHttpOpen(L"BoatOfDaphnes (The Wind Waker PC port; player model browser)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return failed("no network session");
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);
    HINTERNET connection = WinHttpConnect(session, host, u.nPort, 0);
    HINTERNET req = nullptr;
    if (connection) {
        const std::wstring object = std::wstring(u.lpszUrlPath, u.dwUrlPathLength) + std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
        req = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    bool ok = false;
    const char* why = "the server did not answer";
    std::string pin;
    const bool pinned = req && u.nScheme == INTERNET_SCHEME_HTTPS && PreparePin(req, narrow(host), &pin);
    bool pin_failed = false;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        !(pinned && (pin_failed = !PinMatches(req, pin))) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        wchar_t length[32] = L"";
        DWORD length_size = sizeof length;
        uint64_t total = 0;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &length_size,
                                WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(length, nullptr, 10);
        if (status != 200) {
            why = "the server refused the file";
        } else if (FILE* f = _wfopen(widen(path).c_str(), L"wb")) {
            uint64_t got = 0;
            ok = true;
            std::vector<uint8_t> chunk(1 << 16);
            for (;;) {
                DWORD read = 0;
                if (!WinHttpReadData(req, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                    ok = false, why = "the download broke off";
                    break;
                }
                if (read == 0)
                    break;
                if (fwrite(chunk.data(), 1, read, f) != read) {
                    ok = false, why = "the file could not be written (is the drive full?)";
                    break;
                }
                got += read;
                if (progress && !progress(got, total)) {
                    ok = false, why = "cancelled";
                    break;
                }
            }
            fclose(f);
            if (ok && total != 0 && got != total)
                ok = false, why = "the download broke off";
        } else {
            why = "the file could not be made";
        }
    }
    if (req)
        WinHttpCloseHandle(req);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok ? true : failed(pin_failed ? "the server's certificate does not match the pinned key" : why);
}

HttpResult HttpRequest(const std::string& base, const char* method, const std::string& path, const std::vector<uint8_t>* body,
                       size_t max_response) {
    HttpResult r;
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    wchar_t host[256];
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.dwSchemeLength = static_cast<DWORD>(-1);
    u.dwUrlPathLength = static_cast<DWORD>(-1);   // pointers into the URL for the path and what follows it
    u.dwExtraInfoLength = static_cast<DWORD>(-1);
    const std::wstring url = widen(base + path);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &u))
        return r;
    HINTERNET session = WinHttpOpen(L"boat-of-daphnes", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return r;
    WinHttpSetTimeouts(session, 8000, 8000, 30000, 60000);
    HINTERNET connection = WinHttpConnect(session, host, u.nPort, 0);
    HINTERNET req = nullptr;
    if (connection) {
        const std::wstring object = std::wstring(u.lpszUrlPath, u.dwUrlPathLength) + std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
        req = WinHttpOpenRequest(connection, widen(method).c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    std::string pin;
    const bool pinned = req && u.nScheme == INTERNET_SCHEME_HTTPS && PreparePin(req, narrow(host), &pin);
    bool sent = false;
    if (req && pinned) {
        // Only the headers go out before the key is checked; the body follows once it matches.
        const DWORD n = body ? static_cast<DWORD>(body->size()) : 0;
        sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, n, 0) != FALSE;
        if (sent && !PinMatches(req, pin)) {
            sent = false;
            std::fprintf(stderr, "[net] %s: the server's certificate does not match the pinned key; connection refused\n", narrow(host).c_str());
        } else if (sent && body) {
            sent = WriteBody(req, *body);
        }
    } else if (req) {
        sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  body ? const_cast<uint8_t*>(body->data()) : WINHTTP_NO_REQUEST_DATA,
                                  body ? static_cast<DWORD>(body->size()) : 0, body ? static_cast<DWORD>(body->size()) : 0, 0) != FALSE;
    }
    if (sent && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);
        r.ok = true;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
                break;
            if (r.body.size() + avail > max_response) {
                r.ok = false; // larger than it may be
                break;
            }
            const size_t at = r.body.size();
            r.body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, r.body.data() + at, avail, &got)) {
                r.ok = false;
                break;
            }
            r.body.resize(at + got);
        }
    }
    if (req)
        WinHttpCloseHandle(req);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return r;
}

// --- WebSocket --------------------------------------------------------------------------------------------------

struct WebSocket::Impl {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    std::atomic<HINTERNET> socket{nullptr};
};

WebSocket::WebSocket() : mImpl(new Impl) {}

WebSocket::~WebSocket() {
    Close();
    if (mImpl->connect)
        WinHttpCloseHandle(mImpl->connect);
    if (mImpl->session)
        WinHttpCloseHandle(mImpl->session);
    delete mImpl;
}

bool WebSocket::Connect(const std::string& host, unsigned short port, bool secure, const std::string& path, std::string* what,
                        unsigned long* code) {
    auto fail = [&](const char* text) {
        *what = text;
        *code = GetLastError();
        return false;
    };
    mImpl->session = WinHttpOpen(L"boat-of-daphnes/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (mImpl->session == nullptr)
        return fail("cannot start WinHTTP");
    // Connect, send and receive give up after 10 s instead of WinHTTP's 30-60 s.
    WinHttpSetTimeouts(mImpl->session, 10000, 10000, 10000, 10000);
    mImpl->connect = WinHttpConnect(mImpl->session, widen(host).c_str(), port, 0);
    const std::wstring wpath = widen(path);
    if (std::string pin; secure && mImpl->connect && pins::Find(host, &pin)) {
        const int checked = PinPreflight(mImpl->connect, pin);
        if (checked == 2) {
            fail("the server's certificate does not match the pinned key");
            *code = ERROR_WINHTTP_SECURE_INVALID_CERT;
            return false;
        }
        // (1: no answer; the real request below fails the same way and reports it.)
    }
    HINTERNET request = mImpl->connect ? WinHttpOpenRequest(mImpl->connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                       : nullptr;
    HINTERNET socket = nullptr;
    bool ok = false;
    if (std::string loosen; request != nullptr && secure)
        PreparePin(request, host, &loosen);
    if (request == nullptr) {
        fail("cannot reach the server");
    } else if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        fail("the server did not answer");
    } else if (std::string pin; !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        fail("the server did not answer");
    } else if (secure && pins::Find(host, &pin) && !PinMatches(request, pin)) {
        // The handshake is done but the server's key is not the pinned one: nothing is read from it.
        fail("the server's certificate does not match the pinned key");
        *code = ERROR_WINHTTP_SECURE_INVALID_CERT;
    } else if (!WinHttpReceiveResponse(request, nullptr)) {
        fail("the server did not answer");
    } else if ((socket = WinHttpWebSocketCompleteUpgrade(request, 0)) == nullptr) {
        // Not upgraded: the server answered with an HTTP status (429 or 5xx when it is over its limits), which the caller
        // uses to decide on the backup server.
        DWORD status = 0, size = sizeof status;
        const bool have = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
        fail("the server refused the connection");
        if (have)
            *code = status;
    } else {
        ok = true;
    }
    if (request)
        WinHttpCloseHandle(request);
    mImpl->socket = socket;
    return ok;
}

WebSocket::Result WebSocket::Receive(std::string* message) {
    HINTERNET socket = mImpl->socket;
    if (socket == nullptr)
        return Result::Error;
    message->clear();
    std::vector<char> buffer(16 * 1024);
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        if (WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type) != NO_ERROR)
            return Result::Error;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return Result::Closed;
        message->append(buffer.data(), read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            return Result::Text;
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            message->clear(); // reserved for a later pose format
            return Result::Binary;
        }
    }
}

bool WebSocket::SendText(const std::string& text) {
    HINTERNET socket = mImpl->socket;
    return socket != nullptr &&
           WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()),
                                static_cast<DWORD>(text.size())) == NO_ERROR;
}

// Sends the close frame without waiting for the server's (WinHttpWebSocketClose waits).
void WebSocket::Shutdown() {
    HINTERNET socket = mImpl->socket;
    if (socket != nullptr)
        WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
}

// Closing the handle ends the reader's pending receive.
void WebSocket::Close() {
    HINTERNET socket = mImpl->socket.exchange(nullptr);
    if (socket != nullptr)
        WinHttpCloseHandle(socket);
}

// The present USB devices' instance ids ("USB\VID_057E&PID_0337\..."), any class.
bool UsbDevicePresent(unsigned short vid, unsigned short pid) {
    char want[32];
    std::snprintf(want, sizeof want, "VID_%04X&PID_%04X", vid, pid);
    HDEVINFO set = SetupDiGetClassDevsA(nullptr, "USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        char id[512];
        if (SetupDiGetDeviceInstanceIdA(set, &dev, id, sizeof id, nullptr)) {
            for (char* c = id; *c; ++c)
                *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
            found = std::strstr(id, want) != nullptr;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

}  // namespace bodplat
