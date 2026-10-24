"""Every disc format reads the same: opens each given image with tww_disc_check (the project's own reader, src/disc),
and compares main.dol and every file of its file table with the plain ISO's. No disc data is kept.

    python tools/test_disc_formats.py --check <tww_disc_check.exe> --iso <plain .iso> <other image> [<other image> ...]
        [--gcz-temp <folder>]   (no GCZ sample exists: make one from the ISO and test it)

Exits 0 when each image has the ISO's main.dol and the very same files (path, size, SHA-1). Build the tool with
`cmake --build <build dir> --target tww_disc_check`.
"""
import argparse
import os
import struct
import subprocess
import sys
import zlib


def make_gcz(iso, out, block=0x8000):
    """A .gcz of the ISO (Dolphin's layout: header, block offsets, Adler-32s, zlib blocks; stored when compression does not help)."""
    size = os.path.getsize(iso)
    n = (size + block - 1) // block
    offsets, hashes, data, at = [], [], [], 0
    with open(iso, "rb") as f:
        for _ in range(n):
            raw = f.read(block).ljust(block, bytes(1))
            z = zlib.compress(raw, 1)
            stored = len(z) >= len(raw)
            body = raw if stored else z
            offsets.append(at | (1 << 63 if stored else 0))
            hashes.append(zlib.adler32(body))
            data.append(body)
            at += len(body)
    with open(out, "wb") as o:
        o.write(struct.pack("<IIQQII", 0xB10BC001, 0, at, size, block, n))
        o.write(struct.pack(f"<{n}Q", *offsets))
        o.write(struct.pack(f"<{n}I", *hashes))
        for body in data:
            o.write(body)


def describe(tool, image):
    r = subprocess.run([tool, image], capture_output=True, text=True)
    lines = r.stdout.splitlines()
    info = {"format": "?", "main.dol": "", "files": {}}
    for line in lines:
        if line.startswith("format "):
            info["format"] = line[7:]
        elif line.startswith("main.dol "):
            info["main.dol"] = line[9:]
        elif line.startswith("file "):
            path, size, sha = line[5:].rsplit(" ", 2)
            info["files"][path] = (size, sha)
        elif line.startswith("ERROR"):
            info["error"] = line
    if r.returncode != 0 and "error" not in info:
        info["error"] = f"exit {r.returncode}"
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--gcz-temp", help="also make a .gcz of the ISO in this folder (deleted afterwards) and test it")
    ap.add_argument("images", nargs="*")
    a = ap.parse_args()
    gcz = None
    if a.gcz_temp:
        os.makedirs(a.gcz_temp, exist_ok=True)
        gcz = os.path.join(a.gcz_temp, "test.gcz")
        make_gcz(a.iso, gcz)
        a.images.append(gcz)
    ref = describe(a.check, a.iso)
    if "error" in ref or not ref["files"]:
        print("the reference ISO does not read:", ref.get("error"))
        return 1
    print(f"reference: {ref['format']}, {len(ref['files'])} files, main.dol {ref['main.dol']}")
    failed = 0
    for image in a.images:
        got = describe(a.check, image)
        problems = []
        if "error" in got:
            problems.append(got["error"])
        else:
            if got["main.dol"] != ref["main.dol"]:
                problems.append("main.dol differs")
            for path, v in ref["files"].items():
                if got["files"].get(path) != v:
                    problems.append("differs: " + path)
            problems += ["extra: " + p for p in got["files"] if p not in ref["files"]]
        print(f"{'FAIL' if problems else 'ok  '} {got['format']:5} {image}" + (f"  ({len(problems)}: {problems[0]})" if problems else ""))
        failed += bool(problems)
    if gcz:
        os.remove(gcz)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
