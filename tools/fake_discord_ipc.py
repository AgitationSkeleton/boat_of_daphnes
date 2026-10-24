#!/usr/bin/env python3
"""A fake Discord for testing Rich Presence without Discord: listens on a named pipe (Windows) or Unix socket (Linux),
answers the handshake with a READY dispatch, answers every SET_ACTIVITY and prints each one as a line of JSON.

  python tools/fake_discord_ipc.py --pipe bod-test-discord [--log activities.jsonl] [--seconds 120]

Run the game with BLUEWAKE_DISCORD_APP_ID=123 and BLUEWAKE_DISCORD_PIPE=<the pipe's full path>: on Windows
\\\\.\\pipe\\bod-test-discord, on Linux the socket path given as --pipe (e.g. /tmp/bod-test-discord).
Never point a test at the real discord-ipc-N: that is the player's own account.
"""
import argparse
import json
import os
import struct
import sys
import threading
import time


def frame(op, obj):
    data = json.dumps(obj).encode()
    return struct.pack("<II", op, len(data)) + data


def ready():
    return frame(1, {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1, "user": {"id": "0", "username": "fake"}}, "nonce": None})


class Log:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8") if path else None
        self.lock = threading.Lock()

    def line(self, obj):
        text = json.dumps(obj)
        with self.lock:
            print(time.strftime("%H:%M:%S"), text, flush=True)
            if self.f:
                self.f.write(text + "\n")
                self.f.flush()


def serve_client(read, write, log):
    """read(n) -> bytes (b'' on close); write(bytes)."""
    def read_exact(n):
        out = b""
        while len(out) < n:
            part = read(n - len(out))
            if not part:
                return None
            out += part
        return out
    while True:
        head = read_exact(8)
        if head is None:
            log.line({"event": "client closed"})
            return
        op, ln = struct.unpack("<II", head)
        body = read_exact(ln) if ln else b""
        if body is None:
            return
        msg = json.loads(body.decode() or "{}")
        if op == 0:
            log.line({"event": "handshake", "msg": msg})
            write(ready())
        elif op == 1:
            args = msg.get("args", {})
            log.line({"event": "set_activity" if msg.get("cmd") == "SET_ACTIVITY" else msg.get("cmd"),
                      "pid": args.get("pid"), "activity": args.get("activity")})
            write(frame(1, {"cmd": msg.get("cmd"), "data": args.get("activity"), "evt": None, "nonce": msg.get("nonce")}))
        elif op == 3:
            write(frame(4, msg))
        elif op == 2:
            log.line({"event": "close frame"})
            return


def run_windows(name, log, stop):
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateNamedPipeW.restype = wintypes.HANDLE
    k.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    k.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    k.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
    path = name if name.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + name
    log.line({"event": "listening", "pipe": path})
    while not stop.is_set():
        h = k.CreateNamedPipeW(path, 3, 0, 1, 65536, 65536, 0, None)  # duplex, byte mode, one instance
        if h in (None, wintypes.HANDLE(-1).value):
            raise OSError("CreateNamedPipe failed: %d" % ctypes.get_last_error())
        k.ConnectNamedPipe(h, None)
        log.line({"event": "client connected"})

        def rd(n, h=h):
            buf = ctypes.create_string_buffer(n)
            got = wintypes.DWORD(0)
            if not k.ReadFile(h, buf, n, ctypes.byref(got), None) or got.value == 0:
                return b""
            return buf.raw[:got.value]

        def wr(data, h=h):
            got = wintypes.DWORD(0)
            k.WriteFile(h, data, len(data), ctypes.byref(got), None)

        serve_client(rd, wr, log)
        k.DisconnectNamedPipe(h)
        k.CloseHandle(h)


def run_posix(path, log, stop):
    import socket
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)
    srv.settimeout(0.5)
    log.line({"event": "listening", "socket": path})
    while not stop.is_set():
        try:
            c, _ = srv.accept()
        except socket.timeout:
            continue
        log.line({"event": "client connected"})
        serve_client(lambda n: c.recv(n), lambda d: c.sendall(d), log)
        c.close()
    os.unlink(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pipe", required=True, help="pipe name (Windows) or socket path (Linux)")
    ap.add_argument("--log", help="append each event as a JSON line to this file")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: until killed)")
    a = ap.parse_args()
    log = Log(a.log)
    stop = threading.Event()
    if a.seconds:
        threading.Timer(a.seconds, lambda: os._exit(0)).start()
    (run_windows if sys.platform == "win32" else run_posix)(a.pipe, log, stop)


if __name__ == "__main__":
    main()
