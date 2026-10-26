// The certificate pins shared by both platform back ends (platform_win32.cpp, net_posix.cpp): a host name mapped to the
// SHA-256 of its certificate's SubjectPublicKeyInfo (64 hex digits). A pinned host is trusted by that pin alone; any other
// host is validated as usual (the system's CA store).
#pragma once
#include <cctype>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>

namespace bodplat::pins {

inline std::string Normalize(const std::string& s) {
    std::string r;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != ':')
            r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

inline std::mutex& Lock() {
    static std::mutex m;
    return m;
}

inline std::map<std::string, std::string>& Table() {
    static std::map<std::string, std::string> t;
    return t;
}

// Registers (or, with an empty pin, removes) the pin of host. A pin that is not 64 hex digits is ignored.
inline void Set(const std::string& host, const std::string& hex) {
    const std::string h = Normalize(host);
    std::lock_guard<std::mutex> g(Lock());
    if (hex.empty()) {
        Table().erase(h);
        return;
    }
    const std::string p = Normalize(hex);
    if (h.empty() || p.size() != 64 || p.find_first_not_of("0123456789abcdef") != std::string::npos)
        return;
    Table()[h] = p;
}

inline bool Find(const std::string& host, std::string* hex) {
    const std::string h = Normalize(host);
    std::lock_guard<std::mutex> g(Lock());
    const auto it = Table().find(h);
    if (it == Table().end())
        return false;
    *hex = it->second;
    return true;
}

inline std::string ToHex(const unsigned char* d, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string r;
    for (size_t i = 0; i < n; ++i) {
        r += digits[d[i] >> 4];
        r += digits[d[i] & 15];
    }
    return r;
}

inline bool ConstantTimeEqual(const std::string& a, const std::string& b) {
    unsigned diff = a.size() == b.size() ? 0u : 1u;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

}  // namespace bodplat::pins
