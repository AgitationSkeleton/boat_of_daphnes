// Checks Discord's IPC frame encoding (src/bod/windows/src/discord_frame.h) without the game:
//   c++ -std=c++17 -I src/bod/windows/src tools/discord_frame_test.cpp -o /tmp/discord_frame_test && /tmp/discord_frame_test
#include "discord_frame.h"

#include <cstdio>
#include <cstring>

int main() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++bad;
        }
    };
    const std::string json = "{\"v\":1,\"client_id\":\"123\"}";
    const std::string f = discord::EncodeFrame(discord::kHandshake, json);
    check(f.size() == 8 + json.size(), "size");
    const unsigned char expect[8] = {0, 0, 0, 0, static_cast<unsigned char>(json.size()), 0, 0, 0};
    check(std::memcmp(f.data(), expect, 8) == 0, "handshake header");
    check(f.substr(8) == json, "payload");
    uint32_t op = 99, len = 0;
    const std::string g = discord::EncodeFrame(discord::kPong, std::string(300, 'x'));
    check(discord::DecodeHeader(reinterpret_cast<const unsigned char*>(g.data()), &op, &len), "decode ok");
    check(op == discord::kPong && len == 300, "round trip (300 = 0x012c little endian)");
    check(static_cast<unsigned char>(g[4]) == 0x2c && static_cast<unsigned char>(g[5]) == 0x01, "length bytes");
    const unsigned char huge[8] = {1, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f};
    check(!discord::DecodeHeader(huge, &op, &len), "oversize length refused");
    std::printf(bad ? "discord_frame_test: %d failed\n" : "discord_frame_test: ok\n", bad);
    return bad != 0;
}
