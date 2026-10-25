#!/usr/bin/env python3
"""Fetch and verify the offline Python runtime that runs the Wind Waker Randomizer's seed generator.

Output (all git-ignored, under build/windows/rando_runtime/ or, on Linux, build/linux/rando_runtime/; --runtime DIR
chooses another folder):

    python/         Python 3.12: the embeddable package (python.org) with a ._pth on Windows, python-build-standalone's
                    stripped install (astral-sh/python-build-standalone, bin/python3) with a .pth on Linux, either way
                    wiring in the folders below
    site-packages/  pinned pure and binary wheels (ruamel.yaml, Pillow, numpy, ...) plus our imagequant stub
    wwrando/        the generator's sources, copied from a wwrando checkout (never committed here)
    gclib/          LagoLunatic/gclib at the commit wwrando's submodule pointed to (never committed here)
    downloads/      verified download cache
    manifest.json   what was installed, with hashes

Only the Python standard library is used, so any Python 3 can run this script. Idempotent: every step is
stamped with a digest of its inputs and skipped when the output is already current. Every download is
checked against a pinned SHA-256; wwrando and gclib are checked against pinned source-tree digests.

    python scripts/windows/rando_runtime.py            # fetch / update
    python scripts/windows/rando_runtime.py --check    # verify only, exit 0 when current, 1 otherwise
    python scripts/windows/rando_runtime.py --force    # redo every step
    python scripts/windows/rando_runtime.py --wwrando-src D:\\path\\to\\wwrando-master

The wwrando sources come from --wwrando-src, the WWRANDO_SRC environment variable, or the default
read-only snapshot folder; when that is missing or is a different revision, the pinned commit is
downloaded from GitHub instead. See docs/fork/research/rando-seedgen.md.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WINDOWS = sys.platform == "win32"
RUNTIME = REPO / "build" / ("windows" if WINDOWS else "linux") / "rando_runtime"
SHIMS = Path(__file__).resolve().parent / "shims"  # (this repo keeps its scripts/rando in tools/rando)

# ---------------------------------------------------------------------------------------------------
# Pins
# ---------------------------------------------------------------------------------------------------

PYTHON_VERSION = "3.12.10"  # the last 3.12 with Windows binaries; wwrando needs 3.12 (3.14 breaks options/base_options.py)
PYTHON_URL = "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip"
PYTHON_SHA256 = "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"  # matches python.org's SPDX file
# Linux x86-64: python-build-standalone (a relocatable CPython, PSF licence), 3.12 like the wheels below; the stripped
# install_only build (about 34 MB). The SHA-256 is the release's own digest of the file.
PBS_TAG = "20261003"
PBS_VERSION = "3.12.15"
PBS_URL = (f"https://github.com/astral-sh/python-build-standalone/releases/download/{PBS_TAG}/"
           f"cpython-{PBS_VERSION}%2B{PBS_TAG}-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz")
PBS_SHA256 = "731af898886c5f821890dc901eca3c651cca8e51fa7308c159d12a1194aeac91"

# (name, version, filename, url, sha256, license). All are cp312 win_amd64 or pure-Python wheels.
# gclib's requirements.txt at the pinned commit asks for cffi 1.17.1, imagequant 1.1.4, numpy 2.2.3,
# pillow 11.1.0, pycparser 2.22. imagequant (and so cffi, pycparser) is replaced by a stub, see SHIMS.
WHEELS_WINDOWS = [
    ("ruamel.yaml", "0.18.16", "ruamel.yaml-0.18.16-py3-none-any.whl",
     "https://files.pythonhosted.org/packages/0f/73/bb1bc2529f852e7bf64a2dec885e89ff9f5cc7bbf6c9340eed30ff2c69c5/ruamel.yaml-0.18.16-py3-none-any.whl",
     "048f26d64245bae57a4f9ef6feb5b552a386830ef7a826f235ffb804c59efbba", "MIT"),
    ("ruamel.yaml.clib", "0.2.12", "ruamel.yaml.clib-0.2.12-cp312-cp312-win_amd64.whl",
     "https://files.pythonhosted.org/packages/d7/8f/c3654f6f1ddb75daf3922c3d8fc6005b1ab56671ad56ffb874d908bfa668/ruamel.yaml.clib-0.2.12-cp312-cp312-win_amd64.whl",
     "0467c5965282c62203273b838ae77c0d29d7638c8a4e3a1c8bdd3602c10904e4", "MIT"),
    ("appdirs", "1.4.4", "appdirs-1.4.4-py2.py3-none-any.whl",
     "https://files.pythonhosted.org/packages/3b/00/2344469e2084fb287c2e0b57b72910309874c3245463acd6cf5e3db69324/appdirs-1.4.4-py2.py3-none-any.whl",
     "a841dacd6b99318a741b166adb07e19ee71a274450e68237b4650ca1055ab128", "MIT"),
    ("pillow", "11.1.0", "pillow-11.1.0-cp312-cp312-win_amd64.whl",
     "https://files.pythonhosted.org/packages/08/5c/2104299949b9d504baf3f4d35f73dbd14ef31bbd1ddc2c1b66a5b7dfda44/pillow-11.1.0-cp312-cp312-win_amd64.whl",
     "a697cd8ba0383bba3d2d3ada02b34ed268cb548b369943cd349007730c92bddf", "MIT-CMU (HPND)"),
    ("numpy", "2.2.3", "numpy-2.2.3-cp312-cp312-win_amd64.whl",
     "https://files.pythonhosted.org/packages/42/6e/55580a538116d16ae7c9aa17d4edd56e83f42126cb1dfe7a684da7925d2c/numpy-2.2.3-cp312-cp312-win_amd64.whl",
     "83807d445817326b4bcdaaaf8e8e9f1753da04341eceec705c001ff342002e5d", "BSD-3-Clause (+ bundled OpenBLAS/GCC runtime, see its LICENSE)"),
]
# The same versions for Linux x86-64 (manylinux2014, glibc 2.17 and newer; cp312).
WHEELS_LINUX = [
    WHEELS_WINDOWS[0],
    ("ruamel.yaml.clib", "0.2.12", "ruamel.yaml.clib-0.2.12-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
     "https://files.pythonhosted.org/packages/44/d0/3f68a86e006448fb6c005aee66565b9eb89014a70c491d70c08de597f8e4/ruamel.yaml.clib-0.2.12-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
     "95c3829bb364fdb8e0332c9931ecf57d9be3519241323c5274bd82f709cebc0c", "MIT"),
    WHEELS_WINDOWS[2],
    ("pillow", "11.1.0", "pillow-11.1.0-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
     "https://files.pythonhosted.org/packages/8c/aa/7f29711f26680eab0bcd3ecdd6d23ed6bce180d82e3f6380fb7ae35fcf3b/pillow-11.1.0-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
     "7fdadc077553621911f27ce206ffcbec7d3f8d7b50e0da39f10997e8e2bb7f6a", "MIT-CMU (HPND)"),
    ("numpy", "2.2.3", "numpy-2.2.3-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
     "https://files.pythonhosted.org/packages/39/04/78d2e7402fb479d893953fb78fa7045f7deb635ec095b6b4f0260223091a/numpy-2.2.3-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
     "3b787adbf04b0db1967798dba8da1af07e387908ed1553a0d6e74c084d1ceafe", "BSD-3-Clause (+ bundled OpenBLAS/GCC runtime, see its LICENSE)"),
]
WHEELS = WHEELS_WINDOWS if WINDOWS else WHEELS_LINUX

# wwrando snapshot: upstream LagoLunatic/wwrando commit 86b97db521989bf4626870f58fd99c862a46d1d3
# (2025-12-06, "Update ruamel.yaml"), identified by comparing every file of the snapshot folder with the
# upstream history: every file matches (CRLF-insensitive); only the gclib submodule folder is empty. A GitHub
# archive of this commit and the local snapshot give the same tree digest below.
WWRANDO_REPO = "https://github.com/LagoLunatic/wwrando"
WWRANDO_COMMIT = "86b97db521989bf4626870f58fd99c862a46d1d3"
WWRANDO_TREE_SHA256 = "afb637654e865b3f0508f376a9e860343a97627d09f1af3b61fac3db0c0e7d98"
WWRANDO_DEFAULT_SRC = ""  # (no machine path here: a local checkout comes from --wwrando-src or WWRANDO_SRC, else the download)
# What gets copied: the generator and its data. Not the Qt UI, assets, docs, tests or build scripts.
WWRANDO_TOP_FILES = ["randomizer.py", "tweaks.py", "customizer.py", "data_tables.py", "packedbits.py", "version.py",
                     "version.txt", "wwrando_paths.py", "wwrando.py", "LICENSE.txt"]
WWRANDO_DIRS = ["asm", "data", "logic", "options", "randomizers", "seedgen", "wwlib"]
WWRANDO_EXTRA_FILES = ["wwr_ui/inventory.py", "wwr_ui/randomizer_window.ui"]  # the .ui: labels and groups for the menu
# The patching step (scripts/rando/patch_game.py, R2) also reads wwrando's assets (Vscroll.arc, title/banner/icon
# images, sail textures, the blank THP, Link colour masks); a seed does not need them. Own digest and stamp, so the
# generator's pinned tree above is unchanged.
WWRANDO_ASSETS_SHA256 = "ec21b618b48ae4aaa871fb6e78253c5e37b9057b328cf915a142b34003fa8b46"  # 82 files

# gclib: the commit wwrando's submodule pointer named at WWRANDO_COMMIT (2025-05-28,
# "Update reqs_full, add compile_reqs.sh"). MIT.
GCLIB_REPO = "https://github.com/LagoLunatic/gclib"
GCLIB_COMMIT = "abae309e05a8de4c2642e903611e65b4b124e1ce"
GCLIB_TREE_SHA256 = "85353457fdad56522d8fdd43acd056daf8c7f6ae846ceb0807e0a096daaaf39c"

TEXT_EXTS = {".py", ".ui", ".txt", ".md", ".asm", ".cpp", ".h", ".ld", ".bat", ".sh", ".toml", ".json", ".yaml", ".yml", ".cfg"}
SKIP_DIRS = {"__pycache__", ".git", ".github"}


# ---------------------------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------------------------

def log(msg: str) -> None:
    print(f"[rando_runtime] {msg}", flush=True)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree_digest(files: dict[str, bytes]) -> str:
    """Digest of {posix relative path: content}; CRLF is normalised in text files so Windows checkouts match."""
    h = hashlib.sha256()
    for rel in sorted(files):
        data = files[rel]
        if Path(rel).suffix.lower() in TEXT_EXTS:
            data = data.replace(b"\r\n", b"\n")
        h.update(rel.encode() + b"\0" + sha256_bytes(data).encode() + b"\n")
    return h.hexdigest()


def download(url: str, dest: Path, sha256: str) -> Path:
    """Download into the cache (once) and verify; a cached file with a wrong hash is discarded."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and sha256_file(dest) == sha256:
        return dest
    log(f"downloading {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "boat-of-daphnes rando_runtime"})
    with urllib.request.urlopen(req, timeout=120) as resp, open(tmp, "wb") as out:
        shutil.copyfileobj(resp, out)
    got = sha256_file(tmp)
    if got != sha256:
        tmp.unlink()
        raise SystemExit(f"SHA-256 mismatch for {url}\n  expected {sha256}\n  got      {got}")
    tmp.replace(dest)
    return dest


def stamp_ok(stamp: Path, key: str, *outputs: Path) -> bool:
    return stamp.is_file() and stamp.read_text().strip() == key and all(p.exists() for p in outputs)


def write_stamp(stamp: Path, key: str) -> None:
    stamp.write_text(key + "\n")


def collect_dir(root: Path, rel_dirs: list[str], top_files: list[str], extra_files: list[str]) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for name in top_files + extra_files:
        p = root / name
        if p.is_file():
            files[name] = p.read_bytes()
    for d in rel_dirs:
        for p in sorted((root / d).rglob("*")):
            if p.is_file() and not (set(p.relative_to(root).parts) & SKIP_DIRS) and p.suffix != ".pyc":
                files[p.relative_to(root).as_posix()] = p.read_bytes()
    return files


def extract_github_zip(url: str, cache: Path, prefix_hint: str) -> dict[str, bytes]:
    """Download a GitHub source zip (not hash-pinned itself; callers check the tree digest) and return its files."""
    req = urllib.request.Request(url, headers={"User-Agent": "boat-of-daphnes rando_runtime"})
    cache.parent.mkdir(parents=True, exist_ok=True)
    if not cache.exists():
        log(f"downloading {url}")
        with urllib.request.urlopen(req, timeout=300) as resp:
            cache.write_bytes(resp.read())
    files: dict[str, bytes] = {}
    with zipfile.ZipFile(cache) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            parts = info.filename.split("/", 1)
            if len(parts) == 2:
                files[parts[1]] = z.read(info)
    return files


def filter_files(files: dict[str, bytes], rel_dirs: list[str], top_files: list[str], extra_files: list[str]) -> dict[str, bytes]:
    out: dict[str, bytes] = {}
    for rel, data in files.items():
        parts = rel.split("/")
        if set(parts) & SKIP_DIRS or rel.endswith(".pyc"):
            continue
        if rel in top_files or rel in extra_files or (len(parts) > 1 and parts[0] in rel_dirs):
            out[rel] = data
    return out


def write_files(dest: Path, files: dict[str, bytes]) -> None:
    if dest.exists():
        shutil.rmtree(dest)
    for rel, data in files.items():
        p = dest / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


# ---------------------------------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------------------------------

# Linux: a .pth in the interpreter's site-packages (relative to it: rando/python/lib/python3.12/site-packages)
PTH_LINUX = "../../../../site-packages\n../../../../wwrando\n../../../../gclib\n"
PTH = "python312.zip\n.\n..\\site-packages\n..\\wwrando\n..\\gclib\nimport site\n"


def python_exe() -> Path:
    return RUNTIME / "python" / ("python.exe" if WINDOWS else "bin/python3")


def step_python_linux(force: bool) -> None:
    import tarfile
    dest = RUNTIME / "python"
    stamp = RUNTIME / ".stamp-python"
    pth = dest / "lib" / "python3.12" / "site-packages" / "boat-of-daphnes.pth"
    key = sha256_bytes((PBS_SHA256 + PTH_LINUX).encode())
    if not force and stamp_ok(stamp, key, python_exe(), pth):
        return
    tpath = download(PBS_URL, RUNTIME / "downloads" / "python-build-standalone-linux-x86_64.tar.gz", PBS_SHA256)
    if dest.exists():
        shutil.rmtree(dest)
    with tarfile.open(tpath) as t:
        t.extractall(RUNTIME, filter="data")  # (the archive's top folder is python/)
    pth.write_text(PTH_LINUX)
    write_stamp(stamp, key)
    log(f"python {PBS_VERSION} (python-build-standalone {PBS_TAG}) ready")


def step_python(force: bool) -> None:
    if not WINDOWS:
        return step_python_linux(force)
    dest = RUNTIME / "python"
    stamp = RUNTIME / ".stamp-python"
    key = PYTHON_SHA256 + PTH
    if not force and stamp_ok(stamp, sha256_bytes(key.encode()), python_exe(), dest / "python312._pth"):
        return
    zpath = download(PYTHON_URL, RUNTIME / "downloads" / Path(PYTHON_URL).name, PYTHON_SHA256)
    if dest.exists():
        shutil.rmtree(dest)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(dest)
    (dest / "python312._pth").write_text(PTH, newline="\r\n")
    write_stamp(stamp, sha256_bytes(key.encode()))
    log(f"python {PYTHON_VERSION} ready")


def step_wheels(force: bool) -> None:
    dest = RUNTIME / "site-packages"
    stamp = RUNTIME / ".stamp-wheels"
    key = sha256_bytes("".join(w[4] for w in WHEELS).encode())
    if not force and stamp_ok(stamp, key, dest / "ruamel", dest / "numpy", dest / "PIL"):
        return
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for name, ver, fname, url, sha, _lic in WHEELS:
        whl = download(url, RUNTIME / "downloads" / fname, sha)
        with zipfile.ZipFile(whl) as z:
            z.extractall(dest)
        log(f"installed {name} {ver}")
    write_stamp(stamp, key)


def step_shims(force: bool) -> None:
    dest = RUNTIME / "site-packages"
    stamp = RUNTIME / ".stamp-shims"
    srcs = sorted(SHIMS.glob("*.py"))
    key = sha256_bytes(b"".join(p.name.encode() + p.read_bytes() for p in srcs))
    if not force and stamp_ok(stamp, key) and all((dest / p.name).exists() for p in srcs):
        return
    for p in srcs:
        shutil.copyfile(p, dest / p.name)
        log(f"installed shim {p.name}")
    write_stamp(stamp, key)


def step_gclib(force: bool) -> str:
    dest = RUNTIME / "gclib"
    stamp = RUNTIME / ".stamp-gclib"
    key = GCLIB_COMMIT + GCLIB_TREE_SHA256
    if not force and stamp_ok(stamp, key, dest / "gclib" / "rarc.py"):
        return GCLIB_TREE_SHA256
    files = extract_github_zip(f"{GCLIB_REPO}/archive/{GCLIB_COMMIT}.zip",
                               RUNTIME / "downloads" / f"gclib-{GCLIB_COMMIT}.zip", "gclib")
    files = {k: v for k, v in files.items() if not (set(k.split("/")) & SKIP_DIRS) and not k.endswith(".pyc")}
    digest = tree_digest(files)
    if digest != GCLIB_TREE_SHA256:
        raise SystemExit(f"gclib tree digest mismatch\n  expected {GCLIB_TREE_SHA256}\n  got      {digest}")
    write_files(dest, files)
    write_stamp(stamp, key)
    log(f"gclib {GCLIB_COMMIT[:9]} ready")
    return digest


def resolve_wwrando(src_arg: str | None) -> tuple[dict[str, bytes], str]:
    """Return (files, origin). Prefer a local checkout when it is the pinned revision, else download."""
    candidates = [src_arg, os.environ.get("WWRANDO_SRC"), WWRANDO_DEFAULT_SRC]
    for c in candidates:
        if not c:
            continue
        root = Path(c)
        if not (root / "randomizer.py").is_file():
            continue
        files = collect_dir(root, WWRANDO_DIRS, WWRANDO_TOP_FILES, WWRANDO_EXTRA_FILES)
        digest = tree_digest(files)
        if digest == WWRANDO_TREE_SHA256:
            return files, str(root)
        log(f"{root} is not the pinned wwrando revision (digest {digest[:12]}); ignoring it")
    raw = extract_github_zip(f"{WWRANDO_REPO}/archive/{WWRANDO_COMMIT}.zip",
                             RUNTIME / "downloads" / f"wwrando-{WWRANDO_COMMIT}.zip", "wwrando")
    return filter_files(raw, WWRANDO_DIRS, WWRANDO_TOP_FILES, WWRANDO_EXTRA_FILES), f"{WWRANDO_REPO}/archive/{WWRANDO_COMMIT}.zip"


def step_assets(force: bool, src_arg: str | None) -> None:
    dest = RUNTIME / "wwrando" / "assets"
    stamp = RUNTIME / ".stamp-assets"
    if not force and stamp_ok(stamp, WWRANDO_ASSETS_SHA256, dest / "Vscroll.arc"):
        return
    files = None
    for c in (src_arg, os.environ.get("WWRANDO_SRC"), WWRANDO_DEFAULT_SRC):
        if c and (Path(c) / "assets" / "Vscroll.arc").is_file():
            cand = collect_dir(Path(c), ["assets"], [], [])
            if tree_digest(cand) == WWRANDO_ASSETS_SHA256:
                files = cand
                break
    if files is None:
        raw = extract_github_zip(f"{WWRANDO_REPO}/archive/{WWRANDO_COMMIT}.zip",
                                 RUNTIME / "downloads" / f"wwrando-{WWRANDO_COMMIT}.zip", "wwrando")
        files = filter_files(raw, ["assets"], [], [])
    digest = tree_digest(files)
    if digest != WWRANDO_ASSETS_SHA256:
        raise SystemExit(f"wwrando assets digest mismatch: expected {WWRANDO_ASSETS_SHA256}, got {digest}")
    write_files(dest.parent / "assets", {k.split("/", 1)[1]: v for k, v in files.items()})
    write_stamp(stamp, WWRANDO_ASSETS_SHA256)
    log(f"wwrando assets ready ({len(files)} files)")


def step_wwrando(force: bool, src_arg: str | None) -> tuple[str, str]:
    dest = RUNTIME / "wwrando"
    stamp = RUNTIME / ".stamp-wwrando"
    key = WWRANDO_COMMIT + WWRANDO_TREE_SHA256
    if not force and stamp_ok(stamp, key, dest / "randomizer.py"):
        return WWRANDO_TREE_SHA256, "(cached)"
    files, origin = resolve_wwrando(src_arg)
    digest = tree_digest(files)
    if digest != WWRANDO_TREE_SHA256:
        raise SystemExit(f"wwrando tree digest mismatch\n  expected {WWRANDO_TREE_SHA256}\n  got      {digest}")
    write_files(dest, files)
    write_stamp(stamp, key)
    log(f"wwrando copied from {origin} ({len(files)} files)")
    return digest, origin


def smoke_test() -> str:
    py = python_exe()
    code = ("import sys; import randomizer; from version import VERSION; "
            "print(sys.version.split()[0], VERSION)")
    r = subprocess.run([str(py), "-c", code], cwd=RUNTIME / "wwrando", capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("smoke test failed:\n" + r.stdout + r.stderr)
    return r.stdout.strip()


def write_manifest(wwrando_digest: str, gclib_digest: str, origin: str, smoke: str) -> None:
    manifest = {
        "python": ({"version": PYTHON_VERSION, "url": PYTHON_URL, "sha256": PYTHON_SHA256} if WINDOWS else
                   {"version": PBS_VERSION, "url": PBS_URL, "sha256": PBS_SHA256}),
        "wheels": [{"name": n, "version": v, "file": f, "sha256": s, "license": lic} for n, v, f, _u, s, lic in WHEELS],
        "shims": sorted(p.name for p in SHIMS.glob("*.py")),
        "wwrando": {"repo": WWRANDO_REPO, "commit": WWRANDO_COMMIT, "tree_sha256": wwrando_digest, "copied_from": origin,
                    "license": "MIT"},
        "gclib": {"repo": GCLIB_REPO, "commit": GCLIB_COMMIT, "tree_sha256": gclib_digest, "license": "MIT"},
        "smoke_test": smoke,
    }
    (RUNTIME / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def check() -> bool:
    """True when every step is current and the smoke test passes (no network)."""
    ok = True
    for name, outs in [("python", [python_exe().relative_to(RUNTIME).as_posix()]), ("wheels", ["site-packages/ruamel", "site-packages/numpy"]),
                       ("shims", ["site-packages/imagequant.py"]), ("gclib", ["gclib/gclib/rarc.py"]),
                       ("wwrando", ["wwrando/randomizer.py"]), ("assets", ["wwrando/assets/Vscroll.arc"])]:
        if not (RUNTIME / f".stamp-{name}").is_file() or not all((RUNTIME / o).exists() for o in outs):
            log(f"missing: {name}")
            ok = False
    if ok:
        try:
            log("smoke test: " + smoke_test())
        except SystemExit as e:
            log(str(e))
            ok = False
    return ok


def main() -> int:
    global RUNTIME
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="verify only (no network); exit 1 if the runtime is missing or stale")
    ap.add_argument("--force", action="store_true", help="redo every step")
    ap.add_argument("--wwrando-src", help="a wwrando checkout to copy from (must be the pinned revision)")
    ap.add_argument("--runtime", help=f"the folder to make the runtime in (default {RUNTIME})")
    args = ap.parse_args()
    if args.runtime:
        RUNTIME = Path(args.runtime).resolve()

    if args.check:
        return 0 if check() else 1

    RUNTIME.mkdir(parents=True, exist_ok=True)
    step_python(args.force)
    step_wheels(args.force)
    step_shims(args.force)
    gclib_digest = step_gclib(args.force)
    wwrando_digest, origin = step_wwrando(args.force, args.wwrando_src)
    step_assets(args.force, args.wwrando_src)
    smoke = smoke_test()
    write_manifest(wwrando_digest, gclib_digest, origin, smoke)
    log(f"ready: {smoke}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
