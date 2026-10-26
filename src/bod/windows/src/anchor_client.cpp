// The online play connection: see anchor_client.h.
#include "anchor_client.h"

#include "platform/bod_platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace anchor {
namespace {

std::mutex g_lock;
std::deque<std::string> g_outbox, g_inbox;
std::string g_error;
std::atomic<Status> g_status{Status::Disconnected};

// One Start's connection. Stop only flags it and lets go: its thread closes the socket and ends on its own, so the
// caller (the render thread) never waits on the network. A WebSocket close on a dead link waited ~10 s for the
// server's answer, and the game froze that long at a disconnect (a friend's log, 2026-10-12).
struct Conn {
    std::atomic<bool> run{true};
    Options options;
    unsigned id = 0;
};
// Which server the current connection is on (under g_lock).
bool g_on_fallback = false;
bool g_failover_notice = false;
std::string g_active_host;

std::shared_ptr<Conn> g_conn;
unsigned g_conn_id = 0; // the current connection's id, under g_lock

bool current(const Conn& c) {
    std::lock_guard lock(g_lock);
    return c.id == g_conn_id;
}

void set_status(const Conn& c, Status s) {
    if (current(c))
        g_status = s;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

void fail(const Conn& c, const char* what, unsigned long code) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s (error %lu)", what, code);
    std::lock_guard lock(g_lock);
    if (c.id != g_conn_id)
        return;
    g_error = buf;
    g_status = Status::Failed;
    std::fprintf(stderr, "[anchor] %s\n", buf);
}

// Where a connection goes: the main server or the backup one.
struct Target {
    std::string host;
    unsigned short port;
    bool secure;
};

struct Outcome {
    bool connected = false;  // it was up (and has since dropped, or Stop)
    bool overloaded = false; // the server answered the upgrade with HTTP 429 or 5xx (Cloudflare's limits, or down)
};

// One connection, until it drops or Stop. Returns when the socket is gone.
Outcome session(Conn& c, const Target& target) {
    const Options& options = c.options;
    bodplat::WebSocket socket;
    std::string what;
    unsigned long code = 0;
    Outcome outcome;
    const bool connected = socket.Connect(target.host, target.port, target.secure,
                                          "/ws?room=" + url_encode(options.room), &what, &code);
    outcome.connected = connected;
    if (!connected) {
        // The platform layers put the server's HTTP status in `code` when it refused the upgrade.
        outcome.overloaded = what.find("refused") != std::string::npos && (code == 429 || (code >= 500 && code <= 599));
        fail(c, what.c_str(), code);
    }
    if (connected && !c.run) {
        socket.Close(); // stopped while connecting
        return outcome;
    }
    if (connected) {
        set_status(c, Status::Connected);
        std::fprintf(stderr, "[anchor] connected to %s%s room\n", target.host.c_str(),
                     !options.fallback_host.empty() && target.host == options.fallback_host && target.port == options.fallback_port ? " (backup server)" : "");
        // A reader thread; this thread writes and pings.
        std::atomic<bool> open{true};
        std::thread reader([&] {
            std::string message;
            while (open) {
                const bodplat::WebSocket::Result type = socket.Receive(&message);
                if (type == bodplat::WebSocket::Result::Error || type == bodplat::WebSocket::Result::Closed) {
                    open = false;
                    break;
                }
                if (type == bodplat::WebSocket::Result::Text) {
                    std::lock_guard lock(g_lock);
                    if (c.id == g_conn_id && message != "{\"type\":\"PONG\"}")
                        g_inbox.push_back(std::move(message));
                    message.clear();
                }
            }
        });
        auto last_ping = std::chrono::steady_clock::now();
        while (c.run && open) {
            std::deque<std::string> out;
            {
                std::lock_guard lock(g_lock);
                if (c.id == g_conn_id)
                    out.swap(g_outbox);
            }
            for (const std::string& m : out)
                if (!socket.SendText(m)) {
                    open = false;
                    break;
                }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_ping > std::chrono::seconds(30)) {
                socket.SendText("{\"type\":\"PING\"}");
                last_ping = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        open = false;
        // Send the close frame without waiting for the server's, then let go of the socket, which ends the reader's
        // pending receive.
        socket.Shutdown();
        socket.Close();
        reader.join();
        if (current(c) && g_status == Status::Connected)
            g_status = c.run ? Status::Connecting : Status::Disconnected;
        std::fprintf(stderr, "[anchor] disconnected\n");
    }
    return outcome;
}

// The settings' form of a target ("host" for TLS on 443, "wss://host:port" for TLS elsewhere, else "host:port"), for the HTTP
// calls that follow the connection.
std::string host_text(const Target& t) {
    if (!t.secure)
        return t.host + ":" + std::to_string(t.port);
    return t.port == 443 ? t.host : "wss://" + t.host + ":" + std::to_string(t.port);
}

void publish_target(const Conn& c, bool on_fallback, const Target& t, bool notice) {
    std::lock_guard lock(g_lock);
    if (c.id != g_conn_id)
        return;
    g_on_fallback = on_fallback;
    g_active_host = host_text(t);
    if (notice)
        g_failover_notice = true;
}

void run(std::shared_ptr<Conn> c) {
    const Options& o = c->options;
    const Target primary{o.host, o.port, o.secure};
    const Target backup{o.fallback_host, o.fallback_port, o.fallback_secure};
    const bool have_backup = !o.fallback_host.empty();
    bool on_backup = have_backup && o.force_fallback;
    bool told = false; // the player is told once per Start
    int backoff = 1;
    int primary_fails = 0, backup_fails = 0;
    publish_target(*c, on_backup, on_backup ? backup : primary, false);
    while (c->run) {
        set_status(*c, Status::Connecting);
        const Outcome out = session(*c, on_backup ? backup : primary);
        if (!c->run)
            break;
        if (out.connected) {
            primary_fails = backup_fails = 0;
            backoff = 1;
        } else if (have_backup && !on_backup) {
            // The main server is down or over its limits: after 2 refusals with 429/5xx, or 3 failures of any kind, move
            // to the backup server for the rest of this Start.
            ++primary_fails;
            if (primary_fails >= (out.overloaded ? 2 : 3)) {
                std::fprintf(stderr, "[anchor] the main server (%s) is not usable (%d failures%s): moving to the backup server %s\n",
                             primary.host.c_str(), primary_fails, out.overloaded ? ", over its limits" : "", host_text(backup).c_str());
                on_backup = true;
                backup_fails = 0;
                backoff = 1;
                publish_target(*c, true, backup, !told);
                told = true;
                continue; // straight away
            }
        } else if (on_backup && !o.force_fallback) {
            // The backup is not answering either: after 4 failures try the main server again.
            if (++backup_fails >= 4) {
                std::fprintf(stderr, "[anchor] the backup server is not answering either: trying the main server again\n");
                on_backup = false;
                primary_fails = 0;
                publish_target(*c, false, primary, false);
                continue;
            }
        }
        // Reconnect with a growing wait (1, 2, 4 ... 30 s), checking for Stop.
        for (int i = 0; i < backoff * 10 && c->run; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backoff = std::min(backoff * 2, 30);
    }
}

} // namespace

void Start(const Options& options) {
    Stop();
    auto c = std::make_shared<Conn>();
    c->options = options;
    {
        std::lock_guard lock(g_lock);
        c->id = ++g_conn_id;
        g_error.clear();
        g_on_fallback = false;
        g_failover_notice = false;
        g_active_host.clear();
        g_inbox.clear();
        g_outbox.clear();
    }
    g_status = Status::Connecting;
    g_conn = c;
    std::thread(run, c).detach();
}

void Stop() {
    if (g_conn) {
        g_conn->run = false;
        g_conn.reset();
    }
    {
        std::lock_guard lock(g_lock);
        ++g_conn_id; // the old thread's late status, inbox and outbox use are ignored
        g_outbox.clear();
    }
    g_status = Status::Disconnected;
}

Status GetStatus() { return g_status; }

bool OnFallback() {
    std::lock_guard lock(g_lock);
    return g_on_fallback;
}

std::string ActiveHost() {
    std::lock_guard lock(g_lock);
    return g_active_host;
}

bool TakeFailoverNotice() {
    std::lock_guard lock(g_lock);
    const bool notice = g_failover_notice;
    g_failover_notice = false;
    return notice;
}

std::string LastError() {
    std::lock_guard lock(g_lock);
    return g_error;
}

void Send(const std::string& json) {
    std::lock_guard lock(g_lock);
    g_outbox.push_back(json);
}

std::vector<std::string> Poll() {
    std::lock_guard lock(g_lock);
    std::vector<std::string> out(g_inbox.begin(), g_inbox.end());
    g_inbox.clear();
    return out;
}

} // namespace anchor
