#!/usr/bin/env python3
"""First-launch step: the files the package needs that are made from the player's own disc.

    python prepare_extras.py --iso DISC.iso --out GAME_DIR [--timing FILE] [--masks DIR]

The release zip holds nothing made from the game. The app prepares main.dol and the RELs from the disc once
(runtime/host/src/disc_import.c) and then runs this script with the embedded Python (rando\\python\\python.exe) to
make the rest, into the same folder (GAME_DIR, which holds main.dol and rels\\):

    assets/swift_sail/     Swift Sail's red textures      (swift_sail.py, with swift_sail_data.json)
    assets/link_colors/    the Cosmetics Editor's data     (link_colors.py, with wwrando's color masks)
    assets/bunnyhood/      the Bunny Hood's model and icon (bunny_hood.py, with bunny/bunny_hood_params.json)
    rando/rels/            the randomizer variant's RELs   (patch_game.py, wwrando's patcher, the build's
                           reference options and seed, --files code)

These are the very scripts and settings scripts/windows/build.py uses for a personal build, so the files are
identical to the ones it makes. The three steps run side by side. Each step is independent: one that fails leaves
its folder out and says why in GAME_DIR/extras.json, and the others still finish.

Standard output (read by the app's progress window): "PROGRESS <0..1> <text>" lines, then one "RESULT <json>" line.
Exit code 0 when every step worked, 1 when one did not, 2 for bad arguments.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEFAULT_SECONDS = 30.0  # the whole thing on an average PC (13 s on the build PC); the measured time replaces it after the first run


def find_tool(name: str) -> Path:
    """The release keeps the helper scripts beside this one; the source tree keeps them in scripts/windows."""
    for folder in (HERE, REPO / "scripts" / "windows", REPO / "scripts" / "rando"):
        if (folder / name).is_file():
            return folder / name
    raise FileNotFoundError(name)


def runtime_folder() -> Path:
    return Path(os.environ.get("RANDO_RUNTIME") or HERE)


def say(fraction: float, text: str) -> None:
    print(f"PROGRESS {max(0.0, min(1.0, fraction)):.3f} {text}", flush=True)


def run(command: list, env: dict, what: str) -> None:
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, env=env,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode != 0:
        lines = (result.stderr or result.stdout).strip().splitlines()
        raise RuntimeError(f"{what} failed (exit {result.returncode})" + (f": {lines[-1]}" if lines else ""))


def replace_folder(made: Path, target: Path) -> None:
    shutil.rmtree(target, ignore_errors=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(made), str(target))  # (another drive: os.replace would fail)


def step_swift_sail(iso: str, out: Path, env: dict, args) -> str:
    made = out / ".swift_sail.tmp"
    shutil.rmtree(made, ignore_errors=True)
    run([sys.executable, find_tool("swift_sail.py"), iso, made], env, "swift_sail.py")
    count = len(list(made.glob("*.png")))
    if count != 3:
        raise RuntimeError(f"swift_sail.py made {count} textures, not 3")
    replace_folder(made, out / "assets" / "swift_sail")
    return f"{count} textures"


def step_link_colors(iso: str, out: Path, env: dict, args) -> str:
    made = out / ".link_colors.tmp"
    shutil.rmtree(made, ignore_errors=True)
    masks = Path(args.masks) if args.masks else runtime_folder() / "wwrando" / "assets" / "link_color_masks"
    run([sys.executable, find_tool("link_colors.py"), iso, made, "--masks", masks], env, "link_colors.py")
    if not (made / "index.json").is_file():
        raise RuntimeError("link_colors.py wrote no index.json")
    replace_folder(made, out / "assets" / "link_colors")
    return "index.json and textures"


def step_bunny_hood(iso: str, out: Path, env: dict, args) -> str:
    made = out / ".bunny_hood.tmp"
    shutil.rmtree(made, ignore_errors=True)
    run([sys.executable, find_tool("bunny_hood.py"), iso, made], env, "bunny_hood.py")
    for name in ("bunnyhood.arc", "bunnyhood_icon.bti", "bunnyhood_icon.png", "VamuY.arc", "hood.json"):
        if not (made / name).is_file():
            raise RuntimeError(f"bunny_hood.py wrote no {name}")
    replace_folder(made, out / "assets" / "bunnyhood")
    return "model and icon"


def step_rando_rels(iso: str, out: Path, env: dict, args) -> str:
    work = out / ".rando.tmp"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    options = find_tool("reference-options.json")
    seed = "RV1"  # scripts/rando/variant_sites.py REFERENCE_SEED
    run([sys.executable, find_tool("patch_game.py"), "--iso", iso, "--out", work / "ref", "--seed", seed,
         "--options", options, "--files", "code"], dict(env, RANDO_RUNTIME=str(runtime_folder())), "patch_game.py")
    patched = sorted((work / "ref" / "files" / "rels").glob("*.rel"))
    if len(patched) < 80:
        raise RuntimeError(f"the randomizer patcher made {len(patched)} RELs")
    made = work / "rels"
    made.mkdir()
    for path in patched:
        shutil.copy2(path, made / path.name)
    replace_folder(made, out / "rando" / "rels")
    # wwrando's title logo (the same for every seed): the file select shows it for the Randomizer quest
    logo = work / "ref" / "files" / "res" / "Object" / "TlogoE.arc"
    if logo.is_file():
        (out / "rando" / "logo").mkdir(parents=True, exist_ok=True)
        shutil.copy2(logo, out / "rando" / "logo" / "TlogoE.arc")
    shutil.rmtree(work, ignore_errors=True)
    return f"{len(patched)} RELs" + (", the title logo" if logo.is_file() else "")


# What each step leaves under GAME_DIR (relative paths). A step whose outputs are not all there is "missing": the app runs
# --only-missing at every start, so a step added in a later version, or one that failed before, reaches an install made by
# an older one without making the rest again. Keep this list in step with what the step writes.
OUTPUTS = {
    "rando_rels": ["rando/logo/TlogoE.arc"],  # and rando/rels/*.rel, checked by the glob in missing_steps
    "swift_sail": ["assets/swift_sail"],
    "link_colors": ["assets/link_colors/index.json"],
    "bunny_hood": ["assets/bunnyhood/bunnyhood.arc", "assets/bunnyhood/bunnyhood_icon.bti",
                   "assets/bunnyhood/bunnyhood_icon.png", "assets/bunnyhood/VamuY.arc", "assets/bunnyhood/hood.json"],
}


def missing_steps(out: Path) -> list:
    missing = []
    for key, paths in OUTPUTS.items():
        ok = all((out / p).exists() for p in paths)
        if key == "rando_rels":
            ok = ok and len(list((out / "rando" / "rels").glob("*.rel"))) >= 80
        if key == "swift_sail":
            ok = ok and len(list((out / "assets" / "swift_sail").glob("*.png"))) == 3
        if key == "link_colors" and ok:
            # 11d added the swords' and shields' textures (an older install's index has no "equipment" list); the King of
            # Red Lions' hull, figurehead and sail (Ship.arc) came later: an index with no "chars" list (Tetra's and Zelda's parts: hair, skin, eyes, lips, clothes) is older.
            try:
                idx = json.loads((out / "assets" / "link_colors" / "index.json").read_text(encoding="utf-8"))
                ok = any(e.get("name") == "ShipHull" for e in idx.get("equipment") or []) and any(
                    p.get("id") == "Equip.Zelda.Eyeshadow" for c in idx.get("chars") or [] for p in c.get("parts") or [])
            except (OSError, ValueError):
                ok = False
        if key == "bunny_hood" and ok:
            # extras_format 2: VamuY.arc's hood model sits inside the BDLM directory node (11d's files put it outside, so the game
            # could not find it and showed the Hero's Charm's model). 3: the hood's colours are in tev colours 1 and 2 (11d's in
            # 0, which the game's lighting overwrites: a plain white hood). An older hood.json has no such key.
            try:
                ok = int(json.loads((out / "assets" / "bunnyhood" / "hood.json").read_text(encoding="utf-8")).get("extras_format", 0)) >= 3
            except (OSError, ValueError, TypeError):
                ok = False
        if not ok:
            missing.append(key)
    return missing


STEPS = [("rando_rels", "the randomizer's game files", step_rando_rels),
         ("swift_sail", "Swift Sail's textures", step_swift_sail),
         ("link_colors", "Link's colors", step_link_colors),
         ("bunny_hood", "the Bunny Hood", step_bunny_hood)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timing", help="a file that remembers how long this took, for the estimate")
    ap.add_argument("--check", action="store_true", help="print the steps whose outputs are missing, one per line; exit 0 "
                    "when none is, 3 when some are")
    ap.add_argument("--only-missing", action="store_true", help="make only the steps whose outputs are missing (an install "
                    "made by an older version), keeping what extras.json says of the others")
    ap.add_argument("--masks", help="wwrando's link_color_masks folder (default: wwrando/ beside the runtime)")
    args = ap.parse_args()
    out = Path(args.out)
    if not Path(args.iso).is_file() or not out.is_dir():
        print("prepare_extras: the disc or the output folder is missing", file=sys.stderr)
        return 2
    if args.check:
        gone = missing_steps(out)
        for key in gone:
            print(key)
        return 3 if gone else 0
    todo = [step for step in STEPS if step[0] in missing_steps(out)] if args.only_missing else list(STEPS)
    if args.only_missing:
        print("missing from this install: " + (", ".join(k for k, _, _ in todo) or "nothing"), flush=True)
        if not todo:
            return 0
    estimate = DEFAULT_SECONDS
    if args.timing:
        try:
            estimate = max(5.0, float(Path(args.timing).read_text().split()[0]))
        except (OSError, ValueError, IndexError):
            pass
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    results: dict = {}
    if args.only_missing:
        try:
            results = {k: v for k, v in json.loads((out / "extras.json").read_text()).items() if k != "seconds"}
        except (OSError, ValueError):
            results = {}
        for key, _, _ in todo:
            results.pop(key, None)
    start = time.monotonic()

    def work(key: str, label: str, fn) -> None:
        began = time.monotonic()
        try:
            detail = fn(args.iso, out, env, args)
            results[key] = {"ok": True, "detail": detail, "seconds": round(time.monotonic() - began, 1)}
        except Exception as e:  # one step failing must not stop the others
            results[key] = {"ok": False, "error": str(e), "seconds": round(time.monotonic() - began, 1)}

    threads = [threading.Thread(target=work, args=s, daemon=True) for s in todo]
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        elapsed = time.monotonic() - start
        left = max(1, round(estimate - elapsed))
        remaining = [label for key, label, _ in todo if key not in results]
        shown = remaining[0] if remaining else "the last files"
        text = f"Making {shown} from your disc (once), about {left} s left" if elapsed < estimate else \
            f"Making {shown} from your disc (once), nearly done"
        say(min(0.97, elapsed / estimate), text)
        time.sleep(0.5)
    total = time.monotonic() - start
    results["seconds"] = round(total, 1)
    (out / "extras.json").write_text(json.dumps(results, indent=1) + "\n")
    if args.timing and not args.only_missing and all(r["ok"] for k, r in results.items() if k != "seconds"):
        try:
            Path(args.timing).write_text(f"{total:.1f}\n")
        except OSError:
            pass
    say(1.0, "Ready")
    print("RESULT " + json.dumps(results), flush=True)
    return 0 if all(r["ok"] for k, r in results.items() if k != "seconds") else 1


if __name__ == "__main__":
    sys.exit(main())
