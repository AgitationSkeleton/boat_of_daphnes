// Discord's local IPC framing: a 4-byte little-endian opcode, a 4-byte little-endian length, then that many bytes of JSON.
// Header only, so tools/discord_frame_test.cpp can check it without the game.
#pragma once

#include <cstdint>
#include <string>

namespace discord {

enum Opcode : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

inline std::string EncodeFrame(uint32_t op, const std::string& json) {
    const uint32_t len = static_cast<uint32_t>(json.size());
    std::string out(8, '\0');
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<char>((op >> (8 * i)) & 0xff);
        out[4 + i] = static_cast<char>((len >> (8 * i)) & 0xff);
    }
    return out + json;
}

// The 8-byte header at `h`; false when the length is not one Discord sends (a frame is at most 64 KiB).
inline bool DecodeHeader(const unsigned char* h, uint32_t* op, uint32_t* len) {
    *op = uint32_t(h[0]) | uint32_t(h[1]) << 8 | uint32_t(h[2]) << 16 | uint32_t(h[3]) << 24;
    *len = uint32_t(h[4]) | uint32_t(h[5]) << 8 | uint32_t(h[6]) << 16 | uint32_t(h[7]) << 24;
    return *len <= 65536;
}

} // namespace discord
