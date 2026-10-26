// The online play connection (Network > Anchor): one WebSocket to the room
// server (server/anchor, PROTOCOL.md), on its own thread with the platform layer's
// WebSocket client (WinHTTP on Windows, sockets and OpenSSL on Linux). Messages are JSON text; the game side sends and polls.
#pragma once

#include <string>
#include <vector>

namespace anchor {

enum class Status { Disconnected, Connecting, Connected, Failed };

struct Options {
    std::string host = "wwserver.viosarcade.xyz";
    unsigned short port = 443;
    bool secure = true;
    std::string room;
    // The backup server (a self-hosted copy of the room server; server/anchor-node). Empty host: none. The client moves to it
    // when the main server cannot be reached or answers that it is over its limits (HTTP 429/5xx), and stays on it until
    // the next Start. force_fallback: use it from the start (Network > Anchor > Use Backup Server).
    std::string fallback_host;
    unsigned short fallback_port = 80;
    bool fallback_secure = false;
    bool force_fallback = false;
};

// Starts (or restarts) the connection; it reconnects by itself until Stop.
void Start(const Options& options);
void Stop();
Status GetStatus();
std::string LastError();

// Whether the connection is on the backup server, and the host it is on in the settings' form ("host" for the main
// server over TLS, "host:port" for a server without it), so content and other HTTP calls go to the same server.
bool OnFallback();
std::string ActiveHost();
// True once after the connection moves to the backup server on its own (to tell the player).
bool TakeFailoverNotice();

// Queues a message (one JSON object) to send.
void Send(const std::string& json);
// Takes the messages received since the last call.
std::vector<std::string> Poll();

} // namespace anchor
