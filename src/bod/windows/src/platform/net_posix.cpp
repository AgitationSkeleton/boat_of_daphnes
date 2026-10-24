// The Linux side of the platform layer's network code (bod_platform.h): sockets, TLS by the system's OpenSSL, an HTTP/1.1
// client and a WebSocket (RFC 6455) client, for Anchor's connection (anchor_client.cpp) and its content transfers
// (anchor_content.cpp). Windows uses WinHTTP for the same (platform_win32.cpp).
//
// One connection is a Stream: a non-blocking socket, TLS on top for https and wss. A reader thread and a writer thread may
// share one (a WebSocket's receive loop and its sends): each call to OpenSSL takes the Stream's lock, and a reader that
// has nothing to read waits in poll() without holding it.
#include "bod_platform.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace bodplat {

bool Sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
    unsigned int length = 0;
    return EVP_Digest(data, size, out, &length, EVP_sha256(), nullptr) == 1 && length == 32;
}

namespace {

using Clock = std::chrono::steady_clock;

int ms_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left < 0 ? 0 : static_cast<int>(std::min<long long>(left, 1 << 30));
}

SSL_CTX* tls_context() {
    static std::once_flag once;
    static SSL_CTX* ctx = nullptr;
    std::call_once(once, [] {
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr)
            return;
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
        SSL_CTX_set_default_verify_paths(ctx);
    });
    return ctx;
}

// A connection. Read and WriteAll may run on different threads; Close ends a Read in progress.
class Stream {
public:
    ~Stream() {
        Close();
        if (mFd >= 0)
            ::close(mFd);
    }

    bool Open(const std::string& host, unsigned short port, bool secure, int connect_timeout_ms, std::string* what,
              unsigned long* code) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(connect_timeout_ms);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        const std::string service = std::to_string(port);
        const int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &found);
        if (gai != 0 || found == nullptr) {
            *what = "cannot reach the server";
            *code = static_cast<unsigned long>(gai < 0 ? -gai : gai);
            return false;
        }
        int last_errno = ECONNREFUSED;
        for (addrinfo* a = found; a != nullptr && mFd < 0; a = a->ai_next) {
            const int fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
            if (fd < 0) {
                last_errno = errno;
                continue;
            }
            int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
            if (rc != 0 && errno == EINPROGRESS) {
                pollfd p{fd, POLLOUT, 0};
                rc = ::poll(&p, 1, ms_left(deadline)) == 1 ? 0 : -1;
                if (rc == 0) {
                    int err = 0;
                    socklen_t len = sizeof err;
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0) {
                        errno = err;
                        rc = -1;
                    }
                } else {
                    errno = ETIMEDOUT;
                }
            }
            if (rc != 0) {
                last_errno = errno;
                ::close(fd);
                continue;
            }
            mFd = fd;
        }
        freeaddrinfo(found);
        if (mFd < 0) {
            *what = "cannot reach the server";
            *code = static_cast<unsigned long>(last_errno);
            return false;
        }
        const int one = 1;
        setsockopt(mFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (!secure)
            return true;
        SSL_CTX* ctx = tls_context();
        mSsl = ctx != nullptr ? SSL_new(ctx) : nullptr;
        if (mSsl == nullptr) {
            *what = "cannot start TLS";
            *code = ERR_get_error();
            return false;
        }
        SSL_set_fd(mSsl, mFd);
        unsigned char address[sizeof(in6_addr)];
        if (inet_pton(AF_INET, host.c_str(), address) == 1 || inet_pton(AF_INET6, host.c_str(), address) == 1) {
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(mSsl), host.c_str());
        } else {
            SSL_set_tlsext_host_name(mSsl, host.c_str());
            SSL_set1_host(mSsl, host.c_str());
        }
        for (;;) {
            const int rc = SSL_connect(mSsl);
            if (rc == 1)
                return true;
            const int err = SSL_get_error(mSsl, rc);
            if ((err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) || ms_left(deadline) == 0) {
                *what = "the server's certificate or TLS handshake was refused";
                *code = ERR_get_error();
                return false;
            }
            pollfd p{mFd, static_cast<short>(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 0};
            ::poll(&p, 1, ms_left(deadline));
        }
    }

    // The bytes read (more than 0), 0 at the end of the stream, -1 on an error or after Close, -2 when timeout_ms (below
    // 0: none) passed with nothing.
    long Read(void* data, size_t capacity, int timeout_ms) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
        for (;;) {
            if (mClosed)
                return -1;
            short wait_for = POLLIN;
            {
                std::lock_guard<std::mutex> lock(mLock);
                if (mClosed)
                    return -1;
                if (mSsl != nullptr) {
                    const int rc = SSL_read(mSsl, data, static_cast<int>(std::min<size_t>(capacity, 1 << 20)));
                    if (rc > 0)
                        return rc;
                    const int err = SSL_get_error(mSsl, rc);
                    if (err == SSL_ERROR_ZERO_RETURN)
                        return 0;
                    if (err == SSL_ERROR_WANT_WRITE)
                        wait_for = POLLOUT;
                    else if (err != SSL_ERROR_WANT_READ)
                        return (err == SSL_ERROR_SYSCALL && rc == 0) ? 0 : -1;
                } else {
                    const ssize_t rc = ::recv(mFd, data, capacity, 0);
                    if (rc > 0)
                        return rc;
                    if (rc == 0)
                        return 0;
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        return -1;
                }
            }
            if (timeout_ms >= 0 && ms_left(deadline) == 0)
                return -2;
            // Wake often enough to notice Close.
            pollfd p{mFd, wait_for, 0};
            ::poll(&p, 1, timeout_ms < 0 ? 250 : std::min(250, ms_left(deadline)));
        }
    }

    bool WriteAll(const void* data, size_t size) {
        std::lock_guard<std::mutex> frame(mWriteLock);
        const char* at = static_cast<const char*>(data);
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (size > 0) {
            if (mClosed)
                return false;
            short wait_for = POLLOUT;
            {
                std::lock_guard<std::mutex> lock(mLock);
                if (mClosed)
                    return false;
                if (mSsl != nullptr) {
                    const int rc = SSL_write(mSsl, at, static_cast<int>(std::min<size_t>(size, 1 << 20)));
                    if (rc > 0) {
                        at += rc;
                        size -= static_cast<size_t>(rc);
                        continue;
                    }
                    const int err = SSL_get_error(mSsl, rc);
                    if (err == SSL_ERROR_WANT_READ)
                        wait_for = POLLIN;
                    else if (err != SSL_ERROR_WANT_WRITE)
                        return false;
                } else {
                    const ssize_t rc = ::send(mFd, at, size, MSG_NOSIGNAL);
                    if (rc > 0) {
                        at += rc;
                        size -= static_cast<size_t>(rc);
                        continue;
                    }
                    if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        return false;
                }
            }
            if (ms_left(deadline) == 0)
                return false;
            pollfd p{mFd, wait_for, 0};
            ::poll(&p, 1, std::min(250, ms_left(deadline)));
        }
        return true;
    }

    // Ends the connection now; a Read on another thread returns -1. The descriptor stays until the destructor.
    void Close() {
        mClosed = true;
        if (mFd >= 0)
            ::shutdown(mFd, SHUT_RDWR);
        std::lock_guard<std::mutex> lock(mLock);
        if (mSsl != nullptr) {
            SSL_free(mSsl);
            mSsl = nullptr;
        }
    }

private:
    int mFd = -1;
    SSL* mSsl = nullptr;
    std::atomic<bool> mClosed{false};
    std::mutex mLock;       // around each call into OpenSSL
    std::mutex mWriteLock;  // one writer at a time, so a frame goes out whole
};

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The header block's line "name: value", the value; "" when absent. `block` is the text after the status line.
std::string header_value(const std::string& block, const char* name) {
    const std::string want = lower(name) + ":";
    size_t at = 0;
    while (at < block.size()) {
        size_t end = block.find("\r\n", at);
        if (end == std::string::npos)
            end = block.size();
        const std::string line = block.substr(at, end - at);
        if (lower(line.substr(0, want.size())) == want) {
            size_t v = want.size();
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                ++v;
            return line.substr(v);
        }
        at = end + 2;
    }
    return {};
}

struct Url {
    bool secure = false;
    std::string host;
    unsigned short port = 80;
};

bool parse_base(const std::string& base, Url* u) {
    size_t at = 0;
    if (base.rfind("https://", 0) == 0) {
        u->secure = true;
        u->port = 443;
        at = 8;
    } else if (base.rfind("http://", 0) == 0) {
        at = 7;
    } else {
        return false;
    }
    std::string authority = base.substr(at);
    const size_t slash = authority.find('/');
    if (slash != std::string::npos)
        authority.resize(slash);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        u->port = static_cast<unsigned short>(std::atoi(authority.c_str() + colon + 1));
        authority.resize(colon);
    }
    u->host = authority;
    return !u->host.empty();
}

// Reads until the blank line ending the header block; `rest` gets whatever came after it.
bool read_headers(Stream& s, std::string* head, std::string* rest, int timeout_ms) {
    std::string buffer;
    char chunk[4096];
    for (;;) {
        const size_t end = buffer.find("\r\n\r\n");
        if (end != std::string::npos) {
            *head = buffer.substr(0, end);
            *rest = buffer.substr(end + 4);
            return true;
        }
        if (buffer.size() > 64 * 1024)
            return false;
        const long got = s.Read(chunk, sizeof chunk, timeout_ms);
        if (got <= 0)
            return false;
        buffer.append(chunk, static_cast<size_t>(got));
    }
}

}  // namespace

// --- HTTP -------------------------------------------------------------------------------------------------------

// A GET of url streamed to a file (the player model browser's downloads), redirects followed (GameBanana's files come from
// another host), progress reported as the body arrives. https and http; chunked or with a length.
bool HttpDownload(const std::string& url_text, const std::string& path, const std::function<bool(uint64_t, uint64_t)>& progress,
                  std::string* error) {
    auto failed = [&](const char* why) {
        if (error)
            *error = why;
        std::remove(path.c_str());
        return false;
    };
    std::string current = url_text;
    for (int hops = 0; hops < 8; ++hops) {
        // base ("https://host:port") and the rest ("/path?query")
        const size_t scheme = current.find("://");
        if (scheme == std::string::npos)
            return failed("not a URL");
        size_t slash = current.find('/', scheme + 3);
        const std::string base = current.substr(0, slash == std::string::npos ? current.size() : slash);
        std::string object = slash == std::string::npos ? "/" : current.substr(slash);
        const size_t fragment = object.find('#');
        if (fragment != std::string::npos)
            object.resize(fragment);
        Url url;
        if (!parse_base(base, &url))
            return failed("not a URL");
        Stream s;
        std::string what;
        unsigned long code = 0;
        if (!s.Open(url.host, url.port, url.secure, 10000, &what, &code))
            return failed("the server did not answer");
        std::string request = "GET " + object + " HTTP/1.1\r\nHost: " + url.host;
        if (url.port != (url.secure ? 443 : 80))
            request += ":" + std::to_string(url.port);
        request += "\r\nUser-Agent: BoatOfDaphnes (The Wind Waker PC port; player model browser)\r\nAccept: */*\r\n"
                   "Connection: close\r\n\r\n";
        if (!s.WriteAll(request.data(), request.size()))
            return failed("the server did not answer");
        std::string head, rest;
        if (!read_headers(s, &head, &rest, 30000))
            return failed("the server did not answer");
        int status = 0;
        if (std::sscanf(head.c_str(), "HTTP/%*d.%*d %d", &status) != 1)
            return failed("the server did not answer");
        const size_t line_end = head.find("\r\n");
        const std::string fields = line_end == std::string::npos ? std::string() : head.substr(line_end + 2);
        if (status >= 300 && status < 400) {
            std::string location = header_value(fields, "Location");
            if (location.empty())
                return failed("the server refused the file");
            if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0)
                current = location;
            else if (location[0] == '/')
                current = base + location;
            else
                current = base + object.substr(0, object.rfind('/') + 1) + location;
            continue;
        }
        if (status != 200)
            return failed("the server refused the file");
        const bool chunked = lower(header_value(fields, "Transfer-Encoding")).find("chunked") != std::string::npos;
        const std::string length_text = header_value(fields, "Content-Length");
        const uint64_t total = (!chunked && !length_text.empty()) ? std::strtoull(length_text.c_str(), nullptr, 10) : 0;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return failed("the file could not be made");
        uint64_t got = 0;
        const char* why = nullptr;
        std::string pending = rest;  // bytes read and not yet used
        char chunk[1 << 16];
        auto more = [&]() {
            const long n = s.Read(chunk, sizeof chunk, 30000);
            if (n <= 0)
                return false;
            pending.append(chunk, static_cast<size_t>(n));
            return true;
        };
        auto put = [&](const char* data, size_t n) {
            if (std::fwrite(data, 1, n, f) != n) {
                why = "the file could not be written (is the drive full?)";
                return false;
            }
            got += n;
            if (progress && !progress(got, total)) {
                why = "cancelled";
                return false;
            }
            return true;
        };
        if (chunked) {
            for (;;) {
                size_t eol;
                while ((eol = pending.find("\r\n")) == std::string::npos)
                    if (!more()) {
                        why = "the download broke off";
                        break;
                    }
                if (why)
                    break;
                const unsigned long size = std::strtoul(pending.c_str(), nullptr, 16);
                pending.erase(0, eol + 2);
                if (size == 0)
                    break;
                size_t left = size;
                while (left > 0 && !why) {
                    if (pending.empty() && !more()) {
                        why = "the download broke off";
                        break;
                    }
                    const size_t take = std::min(left, pending.size());
                    if (!put(pending.data(), take))
                        break;
                    pending.erase(0, take);
                    left -= take;
                }
                while (!why && pending.size() < 2)
                    if (!more())
                        why = "the download broke off";
                if (why)
                    break;
                pending.erase(0, 2);
            }
        } else {
            if (!pending.empty() && !put(pending.data(), pending.size()))
                pending.clear();
            while (!why) {
                if (total != 0 && got >= total)
                    break;
                const long n = s.Read(chunk, sizeof chunk, 30000);
                if (n == 0)
                    break;
                if (n < 0) {
                    why = "the download broke off";
                    break;
                }
                if (!put(chunk, static_cast<size_t>(n)))
                    break;
            }
            if (!why && total != 0 && got != total)
                why = "the download broke off";
        }
        std::fclose(f);
        return why ? failed(why) : true;
    }
    return failed("too many redirects");
}

HttpResult HttpRequest(const std::string& base, const char* method, const std::string& path, const std::vector<uint8_t>* body,
                       size_t max_response) {
    HttpResult r;
    Url url;
    if (!parse_base(base, &url))
        return r;
    Stream s;
    std::string what;
    unsigned long code = 0;
    if (!s.Open(url.host, url.port, url.secure, 8000, &what, &code))
        return r;
    const bool head_only = std::strcmp(method, "HEAD") == 0;
    std::string request = std::string(method) + " " + (path.empty() ? "/" : path) + " HTTP/1.1\r\nHost: " + url.host;
    if (url.port != (url.secure ? 443 : 80))
        request += ":" + std::to_string(url.port);
    request += "\r\nUser-Agent: boat-of-daphnes\r\nAccept: */*\r\nConnection: close\r\n";
    if (body != nullptr)
        request += "Content-Length: " + std::to_string(body->size()) + "\r\n";
    request += "\r\n";
    if (!s.WriteAll(request.data(), request.size()))
        return r;
    if (body != nullptr && !body->empty() && !s.WriteAll(body->data(), body->size()))
        return r;
    std::string head, rest;
    if (!read_headers(s, &head, &rest, 30000))
        return r;
    int status = 0;
    if (std::sscanf(head.c_str(), "HTTP/%*d.%*d %d", &status) != 1)
        return r;
    const size_t line_end = head.find("\r\n");
    const std::string fields = line_end == std::string::npos ? std::string() : head.substr(line_end + 2);
    r.status = status;
    r.ok = true;
    if (head_only || status == 204 || status == 304 || (status >= 100 && status < 200))
        return r;
    const bool chunked = lower(header_value(fields, "Transfer-Encoding")).find("chunked") != std::string::npos;
    const std::string length_text = header_value(fields, "Content-Length");
    const long long length = length_text.empty() ? -1 : std::atoll(length_text.c_str());
    std::string pending = rest;  // bytes read and not yet used
    char chunk[16384];
    auto more = [&]() {
        const long got = s.Read(chunk, sizeof chunk, 30000);
        if (got <= 0)
            return false;
        pending.append(chunk, static_cast<size_t>(got));
        return true;
    };
    auto add = [&](const char* data, size_t n) {
        if (r.body.size() + n > max_response) {
            r.ok = false; // larger than it may be
            return false;
        }
        r.body.insert(r.body.end(), reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + n);
        return true;
    };
    if (chunked) {
        for (;;) {
            size_t eol;
            while ((eol = pending.find("\r\n")) == std::string::npos)
                if (!more()) {
                    r.ok = false;
                    return r;
                }
            const unsigned long size = std::strtoul(pending.c_str(), nullptr, 16);
            pending.erase(0, eol + 2);
            if (size == 0)
                break;
            size_t left = size;
            while (left > 0) {
                if (pending.empty() && !more()) {
                    r.ok = false;
                    return r;
                }
                const size_t take = std::min(left, pending.size());
                if (!add(pending.data(), take))
                    return r;
                pending.erase(0, take);
                left -= take;
            }
            while (pending.size() < 2)
                if (!more()) {
                    r.ok = false;
                    return r;
                }
            pending.erase(0, 2);
        }
        return r;
    }
    if (!pending.empty() && !add(pending.data(), pending.size()))
        return r;
    for (;;) {
        if (length >= 0 && static_cast<long long>(r.body.size()) >= length)
            break;
        const long got = s.Read(chunk, sizeof chunk, 30000);
        if (got == 0)
            break;
        if (got < 0) {
            r.ok = false;
            break;
        }
        if (!add(chunk, static_cast<size_t>(got)))
            return r;
    }
    if (length >= 0 && static_cast<long long>(r.body.size()) > length)
        r.body.resize(static_cast<size_t>(length));
    return r;
}

// --- WebSocket --------------------------------------------------------------------------------------------------

namespace {

std::string base64(const uint8_t* data, size_t size) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (i + 1 < size ? static_cast<uint32_t>(data[i + 1]) << 8 : 0u) |
                           (i + 2 < size ? data[i + 2] : 0u);
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < size ? table[(v >> 6) & 63] : '=';
        out += i + 2 < size ? table[v & 63] : '=';
    }
    return out;
}

constexpr size_t kMaxMessage = 16u * 1024 * 1024;

}  // namespace

struct WebSocket::Impl {
    Stream stream;
    std::string rx;           // bytes read and not yet parsed
    std::atomic<bool> closed_sent{false};

    bool Fill() {
        char chunk[16384];
        const long got = stream.Read(chunk, sizeof chunk, -1);
        if (got <= 0)
            return false;
        rx.append(chunk, static_cast<size_t>(got));
        return true;
    }

    bool SendFrame(uint8_t opcode, const void* payload, size_t size) {
        std::string frame;
        frame += static_cast<char>(0x80 | opcode);
        if (size < 126) {
            frame += static_cast<char>(0x80 | size);
        } else if (size <= 0xFFFF) {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>(size >> 8);
            frame += static_cast<char>(size & 255);
        } else {
            frame += static_cast<char>(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8)
                frame += static_cast<char>((static_cast<uint64_t>(size) >> shift) & 255);
        }
        uint8_t mask[4];
        if (RAND_bytes(mask, 4) != 1)
            std::memset(mask, 0x5a, 4);
        frame.append(reinterpret_cast<const char*>(mask), 4);
        const size_t at = frame.size();
        frame.append(static_cast<const char*>(payload), size);
        for (size_t i = 0; i < size; ++i)
            frame[at + i] = static_cast<char>(static_cast<uint8_t>(frame[at + i]) ^ mask[i & 3]);
        return stream.WriteAll(frame.data(), frame.size());
    }
};

WebSocket::WebSocket() : mImpl(new Impl) {}

WebSocket::~WebSocket() {
    Close();
    delete mImpl;
}

bool WebSocket::Connect(const std::string& host, unsigned short port, bool secure, const std::string& path, std::string* what,
                        unsigned long* code) {
    if (!mImpl->stream.Open(host, port, secure, 10000, what, code))
        return false;
    uint8_t key_bytes[16];
    if (RAND_bytes(key_bytes, sizeof key_bytes) != 1) {
        *what = "cannot start TLS";
        *code = ERR_get_error();
        return false;
    }
    const std::string key = base64(key_bytes, sizeof key_bytes);
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host;
    if (port != (secure ? 443 : 80))
        request += ":" + std::to_string(port);
    request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
               "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: boat-of-daphnes/1\r\n\r\n";
    std::string head, rest;
    if (!mImpl->stream.WriteAll(request.data(), request.size()) || !read_headers(mImpl->stream, &head, &rest, 10000)) {
        *what = "the server did not answer";
        *code = static_cast<unsigned long>(errno);
        return false;
    }
    int status = 0;
    std::sscanf(head.c_str(), "HTTP/%*d.%*d %d", &status);
    const size_t line_end = head.find("\r\n");
    const std::string fields = line_end == std::string::npos ? std::string() : head.substr(line_end + 2);
    // The accept value proves the server understood the upgrade: base64 of SHA-1(key + the protocol's GUID).
    const std::string joined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    EVP_Digest(joined.data(), joined.size(), digest, &digest_size, EVP_sha1(), nullptr);
    if (status != 101 || header_value(fields, "Sec-WebSocket-Accept") != base64(digest, digest_size)) {
        *what = "the server refused the connection";
        *code = static_cast<unsigned long>(status);
        return false;
    }
    mImpl->rx = rest;
    return true;
}

WebSocket::Result WebSocket::Receive(std::string* message) {
    Impl& w = *mImpl;
    message->clear();
    bool binary = false, in_message = false;
    for (;;) {
        while (w.rx.size() < 2)
            if (!w.Fill())
                return Result::Error;
        const uint8_t b0 = static_cast<uint8_t>(w.rx[0]), b1 = static_cast<uint8_t>(w.rx[1]);
        const bool fin = (b0 & 0x80) != 0;
        const uint8_t opcode = b0 & 15;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t size = b1 & 127;
        size_t header = 2;
        if (size == 126)
            header += 2;
        else if (size == 127)
            header += 8;
        if (masked)
            header += 4;
        while (w.rx.size() < header)
            if (!w.Fill())
                return Result::Error;
        if (size == 126) {
            size = (static_cast<uint64_t>(static_cast<uint8_t>(w.rx[2])) << 8) | static_cast<uint8_t>(w.rx[3]);
        } else if (size == 127) {
            size = 0;
            for (int i = 0; i < 8; ++i)
                size = (size << 8) | static_cast<uint8_t>(w.rx[2 + i]);
        }
        if (size > kMaxMessage)
            return Result::Error;
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked)
            std::memcpy(mask, w.rx.data() + header - 4, 4);
        while (w.rx.size() < header + size)
            if (!w.Fill())
                return Result::Error;
        std::string payload = w.rx.substr(header, static_cast<size_t>(size));
        w.rx.erase(0, header + static_cast<size_t>(size));
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
        switch (opcode) {
        case 9:  // ping: answered with a pong of the same bytes
            w.SendFrame(10, payload.data(), payload.size());
            break;
        case 10:  // pong
            break;
        case 8:
            if (!w.closed_sent.exchange(true))
                w.SendFrame(8, payload.data(), std::min<size_t>(payload.size(), 2));
            return Result::Closed;
        case 1:
        case 2:
        case 0:
            if (opcode != 0) {
                binary = opcode == 2;
                in_message = true;
                message->clear();
            } else if (!in_message) {
                return Result::Error;
            }
            if (message->size() + payload.size() > kMaxMessage)
                return Result::Error;
            message->append(payload);
            if (fin) {
                if (binary) {
                    message->clear(); // reserved for a later pose format
                    return Result::Binary;
                }
                return Result::Text;
            }
            break;
        default:
            return Result::Error;
        }
    }
}

bool WebSocket::SendText(const std::string& text) { return mImpl->SendFrame(1, text.data(), text.size()); }

void WebSocket::Shutdown() {
    if (!mImpl->closed_sent.exchange(true)) {
        const uint8_t status[2] = {0x03, 0xE8};  // 1000, normal closure
        mImpl->SendFrame(8, status, sizeof status);
    }
}

void WebSocket::Close() { mImpl->stream.Close(); }

}  // namespace bodplat
