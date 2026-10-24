#!/usr/bin/env python3
"""Makes the Linux release: dist/boat-of-daphnes-<version>-linux-x86_64.tar.gz, everything the program needs beside it and
nothing of the game. The player unpacks it, starts boat-of-daphnes and picks their disc; the first start builds the game and
the randomizer from it (src/setup/firstrun.cpp) with the bundled compiler (tools/llvm: clang and ld.lld) and the randomizer's
generator (rando/: an embedded Python, wwrando and gclib, the seed and patch scripts). The same rules as tools/package.ps1.

    tools/package.sh [--version 1.0.0] [--build build/linux-clang] [--runtime build/linux/rando_runtime]
                     [--llvm DIR] [--no-tar]

--llvm is a folder holding clang, ld.lld and LICENSE.TXT (default: build/linux/llvm, fetched by tools/linux/fetch_llvm.py).
--runtime is made by tools/rando/rando_runtime.py (run it first). Checks before packing: every file the first start needs is
there, the program's libraries are either beside it or on the list of what every desktop has, and nothing made from a disc is
in the release (game code, game/, the extras the first start makes: assets/link_colors, swift_sail, bunnyhood)."""
import argparse
import glob
import gzip
import os
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Libraries the program may ask of the player's system (every desktop distribution has them); anything else it links must
# be shipped beside it.
SYSTEM_LIBS = {
    "ld-linux-x86-64.so.2", "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libgcc_s.so.1",
    "libstdc++.so.6", "libz.so.1", "libssl.so.3", "libcrypto.so.3", "libpng16.so.16", "libfreetype.so.6",
    "libsqlite3.so.0",  # Aurora takes the system SQLite when one is installed (every desktop distribution has it)
}
FORBIDDEN = ["tww_game.so", "tww_game_rando.so", "tww_game.txt", "tww_game_rando.txt", "tww_game_rando.diff", "game",
             "assets/link_colors", "assets/swift_sail", "assets/bunnyhood", "Save", "saves", "Randomizer", "logs"]
DISC_EXT = {".iso", ".gcm", ".ciso", ".rvz", ".wia", ".rel", ".dol", ".arc", ".bdl", ".bti"}
REQUIRED = ["boat-of-daphnes", "tools/llvm/clang", "tools/llvm/ld.lld", "tools/llvm/LICENSE.TXT", "rando/python/bin/python3",
            "rando/patch_game.py", "rando/generate_seed.py", "rando/bunny_hood.py", "rando/reference-options.json",
            "assets/randomizer/options.json", "assets/fonts/RocknRollOne-Regular.ttf", "BoatOfDaphnes.desktop",
            "boat-of-daphnes.png"]


def needed_libs(path):
    """The shared libraries an ELF file asks for (DT_NEEDED), read with readelf or llvm-readobj."""
    obj = shutil.which("llvm-readobj")
    if obj:
        found = re.findall(r"^\s+(\S+\.so\S*)\s*$", subprocess.run([obj, "--needed-libs", path], capture_output=True, text=True).stdout, re.M)
        if found:
            return found
    for tool, args in (("readelf", ["-d"]), ("llvm-readelf", ["-d"]), ("objdump", ["-p"])):
        exe = shutil.which(tool)
        if not exe:
            continue
        text = subprocess.run([exe, *args, path], capture_output=True, text=True).stdout
        found = re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", text) or re.findall(r"^\s*NEEDED\s+(\S+)", text, re.M)
        if found:
            return found
    raise SystemExit("package: readelf (binutils) is needed to list the program's libraries")


def put(src, out, dest, mode=None):
    if not os.path.exists(src):
        raise SystemExit(f"package: missing: {src} (build it first)")
    d = os.path.join(out, dest)
    os.makedirs(os.path.dirname(d), exist_ok=True)
    if os.path.isdir(src):
        shutil.copytree(src, d, symlinks=True, dirs_exist_ok=True)
    else:
        shutil.copy2(src, d, follow_symlinks=False) if os.path.islink(src) else shutil.copy2(src, d)
        if mode:
            os.chmod(d, mode)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", default="")
    ap.add_argument("--build", default="build/linux-clang")
    ap.add_argument("--runtime", default="build/linux/rando_runtime")
    ap.add_argument("--llvm", default="build/linux/llvm")
    ap.add_argument("--no-tar", action="store_true")
    a = ap.parse_args()
    os.chdir(ROOT)
    version = a.version
    if not version:
        version = subprocess.run(["git", "describe", "--tags", "--always"], capture_output=True, text=True).stdout.strip()
    if not version:
        import datetime
        version = datetime.date.today().strftime("%Y.%m.%d")
    name = f"boat-of-daphnes-{version}-linux-x86_64"
    out = os.path.join("dist", name)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    build, runtime = a.build, a.runtime

    # --- the program and what it loads ---
    exe = os.path.join(build, "boat-of-daphnes")
    put(exe, out, "boat-of-daphnes", 0o755)
    if shutil.which("strip"):  # (debug information only: the dynamic symbols the game's modules bind to stay)
        subprocess.run(["strip", "--strip-debug", os.path.join(out, "boat-of-daphnes")], check=True)
    ships = set()
    for lib in needed_libs(exe):
        if lib in SYSTEM_LIBS:
            continue
        hits = glob.glob(os.path.join(build, "**", lib), recursive=True)
        if not hits:
            raise SystemExit(f"package: the program needs {lib}, which is neither beside it in {build} nor a system library")
        put(os.path.realpath(hits[0]), out, lib)  # (the real file, under the name the program asks for)
        ships.add(lib)
    print("package: libraries beside the program:", ", ".join(sorted(ships)) or "none")

    # --- the compiler the first start builds the game's code with ---
    for f in ("clang", "ld.lld"):
        put(os.path.join(a.llvm, f), out, f"tools/llvm/{f}", 0o755)
    put(os.path.join(a.llvm, "LICENSE.TXT"), out, "tools/llvm/LICENSE.TXT")

    # --- the randomizer's generator ---
    if not os.path.isfile(os.path.join(runtime, "python", "bin", "python3")):
        raise SystemExit(f"package: missing: {runtime}/python/bin/python3 (python3 tools/rando/rando_runtime.py first)")
    for d in ("python", "site-packages", "wwrando", "gclib"):
        put(os.path.join(runtime, d), out, f"rando/{d}")
    put(os.path.join(runtime, "manifest.json"), out, "rando/manifest.json")
    for f in ("generate_seed.py", "patch_game.py", "describe_options.py", "prepare_extras.py", "variant_sites.py", "link_colors.py",
              "swift_sail.py", "swift_sail_data.json", "bunny_hood.py", "check_flags.json"):
        put(os.path.join("tools", "rando", f), out, f"rando/{f}")
    put("tools/rando/bunny", out, "rando/bunny")
    shutil.rmtree(os.path.join(out, "rando", "bunny", "blender"), ignore_errors=True)
    for dirpath, dirnames, _ in os.walk(os.path.join(out, "rando")):
        for d in list(dirnames):
            if d == "__pycache__":
                shutil.rmtree(os.path.join(dirpath, d))
                dirnames.remove(d)
    # wwrando's reference options (the module the first start builds is patched with them) and the menu's option list
    py = os.path.abspath(os.path.join(runtime, "python", "bin", "python3"))
    env = dict(os.environ, PYTHONPATH=os.path.abspath(os.path.join(runtime, "site-packages")), RANDO_RUNTIME=os.path.abspath(runtime))
    ref = os.path.abspath(os.path.join(out, "rando", "reference-options.json"))
    subprocess.run([py, "-c", "import json,sys; sys.path.insert(0, 'tools/rando'); import variant_sites; "
                    f"open({ref!r}, 'w').write(json.dumps(variant_sites.REFERENCE_OPTIONS, indent=1) + '\\n')"], check=True, env=env)
    os.makedirs(os.path.join(out, "assets", "randomizer"), exist_ok=True)
    subprocess.run([py, "tools/rando/describe_options.py", os.path.join(out, "assets", "randomizer", "options.json")], check=True,
                   env=env, stdout=subprocess.DEVNULL)

    # --- assets, folders, papers, the menu entry ---
    put("assets/fonts", out, "assets/fonts")
    os.makedirs(os.path.join(out, "mods"), exist_ok=True)
    with open(os.path.join(out, "mods", "custom_mod_files_go_here.txt"), "w") as f:
        f.write("Mods, texture packs and player models go here (zips or folders). Settings > Mods lists them.\n")
    for f in ("README.md", "LICENSE.md"):
        if os.path.exists(f):
            put(f, out, f)
    if os.path.exists("res/THIRD_PARTY_NOTICES.txt"):
        put("res/THIRD_PARTY_NOTICES.txt", out, "THIRD_PARTY_NOTICES.txt")
    if os.path.exists("src/bod/LICENSE"):
        put("src/bod/LICENSE", out, "licenses/boat-of-daphnes-LICENSE.txt")
    put("assets/fonts/OFL.txt", out, "licenses/RocknRollOne-OFL.txt")
    put("src/bod/windows/resources/BlueWake.png", out, "boat-of-daphnes.png")
    put("tools/linux/BoatOfDaphnes.desktop", out, "BoatOfDaphnes.desktop")
    put("tools/linux/install-menu-entry.sh", out, "install-menu-entry.sh", 0o755)
    put("tools/linux/51-gcadapter.rules", out, "51-gcadapter.rules")

    # --- checks ---
    for f in FORBIDDEN:
        if os.path.exists(os.path.join(out, f)):
            raise SystemExit(f"package: the release must not carry {f} (made from the player's disc)")
    for dirpath, _, files in os.walk(out):
        rel = os.path.relpath(dirpath, out).replace(os.sep, "/")
        if re.match(r"rando/(wwrando|gclib|site-packages|python)(/|$)", rel):
            continue
        for fn in files:
            if os.path.splitext(fn)[1].lower() in DISC_EXT:
                raise SystemExit(f"package: game files in the release: {os.path.join(dirpath, fn)}")
    for f in REQUIRED:
        if not os.path.exists(os.path.join(out, f)):
            raise SystemExit(f"package: missing: {f}")
    files = [os.path.join(d, f) for d, _, fs in os.walk(out) for f in fs]
    print(f"package: {out}: {sum(os.path.getsize(f) for f in files if os.path.isfile(f)) / 2**20:,.0f} MB in {len(files)} files")

    if not a.no_tar:
        tar = os.path.join("dist", name + ".tar.gz")
        if os.path.exists(tar):
            os.remove(tar)

        def fix(ti):
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            if ti.isfile() and (os.path.basename(ti.name) in ("boat-of-daphnes", "clang", "ld.lld") or ti.name.endswith(".sh")):
                ti.mode = 0o755  # (whatever the file system the release was made on says)
            return ti
        with gzip.GzipFile(tar, "wb", compresslevel=9) as gz, tarfile.open(fileobj=gz, mode="w") as t:
            t.add(out, arcname=name, filter=fix)
        print(f"package: {tar}: {os.path.getsize(tar) / 2**20:,.0f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
