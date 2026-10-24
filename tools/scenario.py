#!/usr/bin/env python3
"""Scenario harness: start the game from a chosen save, set up a situation, script it, look at it, check the log.

    python tools/scenario.py windfall_picto_photo                  # a scenario from tools/scenarios/ (or a path)
    python tools/scenario.py --list
    python tools/scenario.py shop_exit_warp --windowed --keep
    python tools/scenario.py --prepare D:\\path\\to\\a\\packaged\\release      # copy the game modules beside the exe

A scenario (tools/scenarios/*.txt, docs/scenarios.md has every command) is a list of commands, one a line, '#' comments:
this tool handles the setup lines (save, patch, cvar, env, options, size, windowed, expect, ...), and the rest is written to
a script the game itself works through (BLUEWAKE_TEST_SCRIPT, src/runtime/bod_testscript.cpp): getting through the title and
file select by what they show, waiting for Link to be playable, save edits, warps, button presses, frame dumps, menu pages.

Output (--out, default build/scenarios): <name>/run.log (the game's log), script.txt, card.raw (the memory card the game
used), shots/*.png (`shot` frame dumps and `winshot` window captures) and result.json (passed, failed expects, peeks).
The exit code is 1 when a scenario fails.

Runs go through the machine-wide game lock (local/bod2/rungame.py when it is there: one game window on this PC, on the
dummy monitor without focus), headless unless the scenario says `windowed`. Saves are never committed: `save NAME` finds
NAME*.gci in the saves folder (--saves, BOD_SAVES, or local/cryze_saves beside the checkout) or takes a path; a Japanese
save is converted to the US game (tools/jp2us.py) and a memory card is built from it (tools/mkcard.py).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
SCENARIOS = TOOLS / "scenarios"
sys.path.insert(0, str(TOOLS))

SETUP_VERBS = {"description", "save", "patch", "size", "windowed", "headless", "cvar", "env", "options", "timeout",
               "expect", "expect_not", "expect_peek", "frames", "rando", "turbo"}


def die(message: str):
    print(f"scenario: {message}", file=sys.stderr)
    sys.exit(2)


def find_local() -> Path | None:
    """The git-ignored local/ folder this checkout lives in (or beside), where the saves and the lock wrapper are."""
    env = os.environ.get("BOD_LOCAL")
    if env:
        return Path(env)
    for p in [ROOT, *ROOT.parents]:
        if (p / "local").is_dir():
            return p / "local"
        if p.name == "local":
            return p
    return None


LOCAL = find_local()


def default_iso() -> str:
    for cand in (os.environ.get("BOD_DISC"), r"D:\Claude_WW\READ_ONLY_GameROMs\Legend of Zelda, The - The Wind Waker (USA).iso"):
        if cand and Path(cand).is_file():
            return cand
    return ""


# ---- the scenario file ----

def parse(path: Path) -> dict:
    s = {"name": path.stem, "file": path, "script": [], "cvars": {}, "env": {}, "patches": [], "expect": [], "expect_not": [],
         "save": None, "options": None, "size": None, "windowed": False, "peeks": [], "turbo": 0, "timeout": 240.0, "frames": 0, "description": ""}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        verb, _, rest = line.partition(" ")
        verb, rest = verb.lower(), rest.strip()
        if verb not in SETUP_VERBS:
            s["script"].append(line)
            continue
        if verb == "description":
            s["description"] = rest
        elif verb == "save":
            s["save"] = rest
        elif verb == "patch":
            s["patches"] += [p for p in rest.replace(" ", ",").split(",") if p]
        elif verb == "size":
            s["size"] = rest
        elif verb == "windowed":
            s["windowed"] = True
        elif verb == "headless":
            s["windowed"] = False
        elif verb == "cvar":
            name, _, value = rest.partition(" ")
            if not value:
                die(f"{path}:{number}: cvar NAME VALUE")
            s["cvars"][name] = value.strip()
        elif verb == "env":
            name, _, value = rest.partition(" ")
            s["env"][name] = value.strip()
        elif verb == "options":
            s["options"] = rest
        elif verb == "timeout":
            s["timeout"] = float(rest)
        elif verb == "frames":
            s["frames"] = int(rest)
        elif verb == "turbo":
            s["turbo"] = int(rest)
        elif verb == "expect_peek":
            m = re.fullmatch(r"(\S+)\s*(==|!=|>=|<=|>|<|&|!&)\s*(\S+)", rest)
            if not m:
                die(f"{path}:{number}: expect_peek LABEL OP VALUE (== != >= <= > < & !&)")
            s["peeks"].append((m.group(1), m.group(2), int(m.group(3), 0)))
        elif verb == "expect":
            s["expect"].append(rest)
        elif verb == "expect_not":
            s["expect_not"].append(rest)
        elif verb == "rando":
            s["rando"] = True
    return s


def find_scenario(name: str) -> Path:
    p = Path(name)
    if p.is_file():
        return p
    for cand in (SCENARIOS / name, SCENARIOS / f"{name}.txt"):
        if cand.is_file():
            return cand
    die(f"no scenario {name} (--list shows the ones in {SCENARIOS})")


# ---- the memory card ----

def saves_dir(arg: str | None) -> Path | None:
    for cand in (arg, os.environ.get("BOD_SAVES"), LOCAL / "cryze_saves" if LOCAL else None):
        if cand and Path(cand).is_dir():
            return Path(cand)
    return None


def find_save(spec: str, folder: Path | None) -> Path:
    p = Path(spec)
    if p.is_file():
        return p
    if folder:
        for cand in (folder / spec, folder / f"{spec}.gci"):
            if cand.is_file():
                return cand
        hits = sorted(folder.glob(f"*{spec}*.gci"))
        if len(hits) == 1:
            return hits[0]
        if hits:
            die(f"save '{spec}' is ambiguous in {folder}: {', '.join(h.name for h in hits)}")
    die(f"no save '{spec}' (a path, or a name in the saves folder {folder}: --saves / BOD_SAVES)")


def patch_save(gci: bytearray, patches: list[str]):
    """Bytes of save file 1 (offsets from the start of the save data, dSv_save_c = 0x803C4C08 in memory), checksums redone
    in both copies. 'off=value' sets the byte, 'off|value' ors it."""
    import jp2us as J
    body = memoryview(gci)[0x40:]
    todo = []
    for p in patches:
        if "|" in p:
            o, v = p.split("|")
            todo.append((int(o, 0), int(v, 0), True))
        else:
            o, v = p.split("=")
            todo.append((int(o, 0), int(v, 0), False))
    for copy in (1, 2):
        base = copy * J.BLOCK
        at = base + J.FILES_AT
        for o, v, orr in todo:
            body[at + o] = (body[at + o] | v) if orr else v
        data = body[at:at + J.FILE_SIZE]
        body[at + J.FILE_SIZE:at + J.FILE_SIZE + 8] = struct.pack(">Q", J.file_checksum(data))
        body[base + J.BLOCK - 4:base + J.BLOCK] = struct.pack(">I", J.block_checksum(body[base:base + J.BLOCK]))


def build_card(s: dict, out: Path, folder: Path | None) -> Path | None:
    """The card.raw the game is pointed at (SA2_CARD): the scenario's save on a formatted card, or none for a new game."""
    card = out / "card.raw"
    if not s["save"] or s["save"].lower() in ("new", "newgame", "none"):
        return None
    src = find_save(s["save"], folder)
    gci = bytearray(src.read_bytes())
    if gci[:4] == b"GZLJ":  # Japanese: the US game's layout and a name its font has
        tmp = out / "save_jp.gci"
        tmp.write_bytes(gci)
        fixed = out / "save.gci"
        subprocess.check_call([sys.executable, str(TOOLS / "jp2us.py"), str(tmp), str(fixed)], stdout=subprocess.DEVNULL)
        tmp.unlink()
        gci = bytearray(fixed.read_bytes())
        fixed.unlink()
    elif gci[:4] != b"GZLE":
        die(f"{src}: not a Wind Waker save ({bytes(gci[:6])})")
    if s["patches"]:
        patch_save(gci, s["patches"])
    gcif = out / "save.gci"
    gcif.write_bytes(gci)
    subprocess.check_call([sys.executable, str(TOOLS / "mkcard.py"), str(card), str(gcif)], stdout=subprocess.DEVNULL)
    gcif.unlink()
    return card


# ---- windows (for `winshot`) ----

def child_pids(parent: int) -> list[int]:
    import ctypes
    import ctypes.wintypes as wt

    class PE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260)]

    k = ctypes.windll.kernel32
    snap = k.CreateToolhelp32Snapshot(2, 0)
    pe = PE()
    pe.dwSize = ctypes.sizeof(PE)
    pairs = []
    ok = k.Process32First(snap, ctypes.byref(pe))
    while ok:
        pairs.append((pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile.decode(errors="replace")))
        ok = k.Process32Next(snap, ctypes.byref(pe))
    k.CloseHandle(snap)
    found, frontier = [], [parent]
    while frontier:
        p = frontier.pop()
        for pid, ppid, name in pairs:
            if ppid == p and pid not in found:
                found.append(pid)
                frontier.append(pid)
    return found


def window_shots(pids: list[int], prefix: Path) -> list[str]:
    """PNGs of every visible top-level window of those processes (PrintWindow): the game's, and the menu's."""
    import ctypes
    import ctypes.wintypes as wt
    from PIL import Image
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    wins = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            wins.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    names = []
    for i, hwnd in enumerate(wins):
        r = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
        if w <= 0 or h <= 0 or w * h > 4000 * 3000:
            continue
        hdc = user32.GetWindowDC(hwnd)
        mdc = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        gdi32.SelectObject(mdc, bmp)
        user32.PrintWindow(hwnd, mdc, 2)
        buf = ctypes.create_string_buffer(w * h * 4)
        bi = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.c_char_p(bi), 0)
        path = Path(f"{prefix}_{i}.png")
        Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB").save(path)
        names.append(path.name)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mdc)
        user32.ReleaseDC(hwnd, hdc)
    return names


def convert_dumps(folder: Path) -> list[str]:
    """The game's raw frame dumps (*.rgba) to PNG."""
    from PIL import Image
    names = []
    for f in sorted(folder.glob("*.rgba")):
        d = f.read_bytes()
        if d[:4] != b"RGBA":
            continue
        w, h, fmt = struct.unpack_from("<3I", d, 4)
        img = Image.frombytes("RGBA", (w, h), d[16:16 + w * h * 4])
        r, g, b, _ = img.split()
        img = Image.merge("RGB", (b, g, r) if fmt == 1 else (r, g, b))
        out = f.with_suffix(".png")
        img.save(out)
        f.unlink()
        names.append(out.name)
    return names


# ---- running ----

def write_json_settings(exe_dir: Path, cvars: dict):
    """boat-of-daphnes.json beside the exe with the scenario's cvars set; the caller restores the original."""
    path = exe_dir / "boat-of-daphnes.json"
    data = json.loads(path.read_text()) if path.is_file() else {}
    root = data.setdefault("CVars", {})
    for name, value in cvars.items():
        node = root
        parts = name.split(".")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        v: object = value
        if re.fullmatch(r"-?\d+", value):
            v = int(value)
        elif re.fullmatch(r"-?\d*\.\d+", value):
            v = float(value)
        node[parts[-1]] = v
    path.write_text(json.dumps(data, indent=4) + "\n")


def run(args, s: dict) -> dict:
    exe = Path(args.exe).resolve()
    if not exe.is_file():
        die(f"no game exe at {exe} (build it, or pass --exe)")
    out = Path(args.out).resolve() / s["name"]
    shutil.rmtree(out, ignore_errors=True)
    (out / "shots").mkdir(parents=True)
    folder = saves_dir(args.saves)
    card = build_card(s, out, folder)
    script = out / "script.txt"
    script.write_text("\n".join(s["script"]) + "\n", encoding="utf-8")
    log = out / "run.log"
    windowed = args.windowed or (s["windowed"] and not args.headless)
    env = {
        "BLUEWAKE_TEST_SCRIPT": str(script), "BLUEWAKE_TEST_SHOTS": str(out / "shots"),
        "BLUEWAKE_OPTIONS": s["options"] if s["options"] is not None else "",
        "SA2_DUMP_MAX": "300",
    }
    # (BLUEWAKE_OPTIONS must be set or the headless run has no options and hooks at all; empty is the defaults)
    if card:
        env["SA2_CARD"] = str(card)
    else:
        env["SA2_CARD"] = str(out / "card.raw")
        env["SA2_CARD_FORMATTED"] = "1"
    if s["size"]:
        env["DOL_AURORA_WINDOW"] = s["size"]
    turbo = s["turbo"] if s["turbo"] else (0 if windowed else int(os.environ.get("BOD_SCENARIO_TURBO", "1")))
    if turbo > 1:  # a headless run is not paced to 60 retraces a second (the game's own clock goes with it)
        env["SA2_TURBO"] = str(turbo)
    env.update(s["env"])
    for item in args.env or []:
        k, _, v = item.partition("=")
        env[k] = v
    game_args = ["--iso", args.iso] + ([] if windowed else ["--headless"])
    frames = args.frames or s["frames"] or 60 * 600
    game_args += ["--frames", str(frames)]
    rungame = Path(args.rungame) if args.rungame else (LOCAL / "bod2" / "rungame.py" if LOCAL else None)
    if rungame and rungame.is_file():
        cmd = [sys.executable, str(rungame), "--exe", str(exe), "--timeout", str(args.timeout or s["timeout"]), "--log", str(log)]
        for k, v in env.items():
            cmd += ["--env", f"{k}={v}"]
        cmd += ["--"] + game_args
        popen_env, cwd, wrapped = None, None, True
    else:  # no lock wrapper here: the game directly (one at a time is the caller's job)
        cmd = [str(exe)] + game_args
        popen_env, cwd, wrapped = {**os.environ, **env}, str(exe.parent), False
    settings = exe.parent / "boat-of-daphnes.json"
    saved = settings.read_bytes() if settings.is_file() else None
    if s["cvars"]:
        write_json_settings(exe.parent, s["cvars"])
    print(f"{s['name']}: {s['description'] or 'running'} ({'windowed' if windowed else 'headless'})")
    t0 = time.time()
    shots_done: set[str] = set()
    dead = 0.0
    try:
        with open(out / ("wrapper.txt" if wrapped else "run.log"), "wb") as stream:
            proc = subprocess.Popen(cmd, env=popen_env, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
            deadline = time.time() + (args.timeout or s["timeout"]) + 60
            while proc.poll() is None:
                time.sleep(0.5)
                if time.time() > deadline and not wrapped:
                    proc.kill()
                text = log.read_text(errors="replace") if log.exists() else ""
                if not dead and re.search(r"Failed assertion|CRASH:|FATAL", text):
                    dead = time.time()  # the game stopped itself: no use holding the game lock; its log says why
                if dead and time.time() > dead + 8:
                    subprocess.call(["taskkill", "/F", "/T", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                for m in re.finditer(r"\[script\] winshot (\S+)", text):
                    key = f"{m.group(1)}@{m.start()}"
                    if key not in shots_done:
                        shots_done.add(key)
                        got = window_shots(child_pids(proc.pid) + [proc.pid], out / "shots" / m.group(1))
                        print(f"  winshot {m.group(1)}: {', '.join(got) or 'no window'}")
            code = proc.returncode
    finally:
        if saved is not None:
            settings.write_bytes(saved)
        elif s["cvars"] and settings.exists():
            settings.unlink()
    return finish(args, s, out, log, code, time.time() - t0)


def finish(args, s: dict, out: Path, log: Path, code, seconds: float) -> dict:
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    shots = convert_dumps(out / "shots") + sorted(p.name for p in (out / "shots").glob("*.png"))
    result = {"scenario": s["name"], "exit": code, "seconds": round(seconds), "shots": sorted(set(shots)), "expect": [],
              "peeks": re.findall(r"\[script\] peek .*", text), "problems": [], "passed": True}
    def check(entry: dict, ok: bool):
        entry["ok"] = ok
        result["expect"].append(entry)
        result["passed"] &= ok

    for pat in s["expect"]:
        check({"log": pat}, re.search(pat, text) is not None)
    for pat in s["expect_not"]:
        check({"not_log": pat}, re.search(pat, text) is None)
    tests = {"==": lambda v, w: v == w, "!=": lambda v, w: v != w, ">=": lambda v, w: v >= w, "<=": lambda v, w: v <= w,
             ">": lambda v, w: v > w, "<": lambda v, w: v < w, "&": lambda v, w: v & w == w, "!&": lambda v, w: v & w == 0}
    for label, op, want in s["peeks"]:
        pat = r"\[script\] peek 0x[0-9A-Fa-f]+/\d+ = 0x([0-9A-Fa-f]+) \(\d+\) " + re.escape(label) + r"(\s|$)"
        vals = [int(m.group(1), 16) for m in re.finditer(pat, text, re.M)]
        check({"peek": f"{label} {op} {want:#x}", "values": [hex(v) for v in vals]},
              bool(vals) and all(tests[op](v, want) for v in vals))
    for pat, what in ((r"\[script\] TIMEOUT[^\n]*", "a wait timed out"), (r"\[script\] [^\n]*unknown command[^\n]*", "unknown command"),
                      (r"\[script\] cannot read[^\n]*", "no script")):
        for m in re.finditer(pat, text):
            result["problems"].append(m.group(0))
    if "[script] done at" not in text and "[script] quit at" not in text:
        result["problems"].append("the script did not run to its end")
    if result["problems"]:
        result["passed"] = False
    if code not in (0, "0"):
        result["problems"].append(f"exit code {code}")
        result["passed"] = False
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"  {'ok' if result['passed'] else 'FAILED'} in {seconds:.0f} s: {out}")
    for p in result["peeks"]:
        print("  " + p)
    for e in result["expect"]:
        if not e["ok"]:
            print(f"  expectation not met: {e}")
    for p in result["problems"]:
        print(f"  problem: {p}")
    if result["shots"]:
        print(f"  shots: {', '.join(result['shots'])}")
    return result


def prepare(player: Path, exe: Path):
    """Copy what a packaged release has beside its exe that a fresh build lacks: the game modules (built from the disc on the
    first start of the program) and their data. They work while the runtime ABI is unchanged (the stamp in tww_game.txt)."""
    dst = exe.parent
    names = ["tww_game.dll", "tww_game.txt", "tww_game_rando.dll", "tww_game_rando.txt", "tww_game_rando.diff"]
    for n in names:
        if (player / n).is_file():
            shutil.copy2(player / n, dst / n)
    for d in ("game", "rando", "assets/link_colors", "assets/swift_sail", "assets/bunnyhood"):
        if (player / d).is_dir():
            shutil.copytree(player / d, dst / d, dirs_exist_ok=True)
    print(f"modules copied from {player} to {dst}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("scenarios", nargs="*", help="names in tools/scenarios (without .txt) or paths")
    p.add_argument("--exe", default=str(ROOT / "build" / "windows-clang" / "boat-of-daphnes.exe"))
    p.add_argument("--iso", default=default_iso(), help="the disc image (env BOD_DISC)")
    p.add_argument("--out", default=str(ROOT / "build" / "scenarios"))
    p.add_argument("--saves", help="folder of .gci saves (env BOD_SAVES; local/cryze_saves)")
    p.add_argument("--rungame", help="the lock wrapper (default local/bod2/rungame.py)")
    p.add_argument("--windowed", action="store_true", help="a window instead of headless (menus, cvars that need it)")
    p.add_argument("--headless", action="store_true", help="headless even when the scenario says windowed")
    p.add_argument("--timeout", type=float, default=0, help="seconds before the game is killed (default: the scenario's, 240)")
    p.add_argument("--frames", type=int, default=0, help="retraces before the run ends whatever the script does")
    p.add_argument("--env", action="append", help="NAME=VALUE for the game")
    p.add_argument("--list", action="store_true")
    p.add_argument("--prepare", metavar="PLAYER_DIR", help="copy the game modules from a packaged release beside the exe, then stop")
    args = p.parse_args()
    if args.list:
        for f in sorted(SCENARIOS.glob("*.txt")):
            print(f"{f.stem:28} {parse(f)['description']}")
        return 0
    if args.prepare:
        prepare(Path(args.prepare), Path(args.exe))
        return 0
    if not args.scenarios:
        p.error("name a scenario (--list shows them)")
    if not args.iso:
        die("no disc image: --iso or the BOD_DISC variable")
    failed = 0
    for name in args.scenarios:
        failed += not run(args, parse(find_scenario(name)))["passed"]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
