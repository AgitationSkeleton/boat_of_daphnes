#!/usr/bin/env python3
"""Produce wwrando's patched game files without writing an ISO (milestone R2, input step).

Runs wwrando's own WWRandomizer in its normal (non dry) mode on the player's disc, but replaces the last step,
which writes the GCM/ISO, with a collector: every file wwrando changed or added is written under --out at its disc
path (sys/main.dol, files/res/Stage/..., files/rels/NAME.rel, ...), and patch_manifest.json lists them with the
vanilla and patched SHA-1 and, for archives, which inner files differ. The disc is only read.

    python scripts/rando/patch_game.py --iso DISC.iso --out DIR --seed SEED [--options opts.json] [--set name=value ...]
    python scripts/rando/patch_game.py --iso DISC.iso --out DIR --permalink PERMALINK

Differences from running wwrando's CLI (all deliberate):
  * no ISO or folder export; only changed files are written (an archive is "changed" when an inner file differs,
    so archives that wwrando merely re-packed are left out);
  * a REL that lives in RELS.arc is written as files/rels/NAME.rel (what the port's game/rels folder holds), and
    RELS.arc itself is not written;
  * wwrando's source-tree "developer mode" DOL byte (0x803F60E0) is not set: this is the release behaviour.

Needs the embedded Python 3.12 runtime: python scripts/windows/rando_runtime.py (it re-launches itself under it).
Exit codes: 0 ok, 2 bad arguments, 3 generation failed, 4 runtime missing or internal error.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import time
import traceback
from pathlib import Path
import sys as _bod_sys
from pathlib import Path as _BodPath
_bod_sys.path.insert(0, str(_BodPath(__file__).resolve().parents[1]))
try:
    from bod_paths import local_path, scratch  # noqa: E402  (scripts/bod_paths.py)
except ImportError:  # a release's copy of this script, without the repository's scripts/ beside it
    import os as _bod_os

    def local_path(name, fallback=""):
        return _bod_os.environ.get(name) or fallback

    def scratch():
        return _BodPath(local_path("BOD_SCRATCH", "scratch"))

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
RUNTIME = Path(os.environ.get("RANDO_RUNTIME") or REPO / "build" / ("windows" if os.name == "nt" else "linux") / "rando_runtime")

if sys.version_info[:2] != (3, 12):  # wwrando needs 3.12: hand over to the embedded one
    import subprocess
    _py = RUNTIME / "python" / ("python.exe" if os.name == "nt" else "bin/python3")
    if not _py.is_file():
        sys.exit(f"patch_game: rando runtime not found at {RUNTIME}; run: python scripts/windows/rando_runtime.py")
    sys.exit(subprocess.run([str(_py), str(Path(__file__).resolve()), *sys.argv[1:]]).returncode)

sys.path.insert(0, str(HERE))
import generate_seed as gs  # noqa: E402  (shares its option parsing and error exits)

DEFAULT_ISO = local_path("BOD_DISC")


def install_quantizer() -> None:
    """The runtime's imagequant is a stub (the real wheel bundles GPL code). Patching re-encodes a few palette
    textures (title logo, banners, key bag, sail art), so give it a Pillow-based quantizer: deterministic and
    licence-clean, but not bit-identical to upstream's output for those images (cosmetic only)."""
    import imagequant
    from PIL import Image

    def quantize_pil_image(image, dithering_level=1.0, max_colors=256, min_quality=0, max_quality=100):
        rgba = image.convert("RGBA")
        return rgba.quantize(colors=max_colors, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)

    imagequant.quantize_pil_image = quantize_pil_image


def prep_rel(data: bytes) -> bytes:
    """What the port's disc import does to every REL it extracts (runtime/host/src/disc_import.c write_rel): the
    alignment word at 0x40 becomes 4, which DolRecomp's section-alignment check needs."""
    return data[:0x40] + (4).to_bytes(4, "big") + data[0x44:] if len(data) >= 0x44 else data


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def as_bytes(f) -> bytes:
    if f is None:
        return b""
    if isinstance(f, (bytes, bytearray)):
        return bytes(f)
    from gclib import fs_helpers as fs
    return fs.read_all_bytes(f)


class Patched:
    """What one wwrando run produced: path -> bytes for changed/added files, plus the manifest rows."""

    def __init__(self):
        self.files: dict[str, bytes] = {}
        self.rows: list[dict] = []
        self.notes: list[str] = []


def collect(rando, vanilla_gcm) -> Patched:
    """Mirror WWRandomizer.save_randomized_iso up to (not including) the GCM export; keep what differs from the disc."""
    from gclib.rarc import RARC
    from gclib.yaz0_yay0 import Yaz0

    res = Patched()
    changed: dict[str, io.BytesIO | None] = {}

    rando.bmg.save_changes()
    for file_path, data in rando.raw_files_by_path.items():
        changed[file_path] = data
    rando.dol.save_changes()
    changed["sys/main.dol"] = rando.dol.data

    rels_arc = rando.get_arc("files/RELS.arc")
    rel_files: dict[str, bytes] = {}
    for rel_path, rel in rando.rels_by_path.items():
        rel.save_changes(preserve_section_data_offsets=True)
        rel_name = os.path.basename(rel_path)
        entry = rels_arc.get_file_entry(rel_name)
        if entry is None:
            changed[rel_path] = rel.data
        rel_files[rel_path] = prep_rel(as_bytes(rel.data))

    for arc_path, arc in rando.arcs_by_path.items():
        for file_name, instantiated_file in arc.instantiated_object_files.items():
            if file_name == "event_list.dat":
                instantiated_file.save_changes()
        arc.save_changes()
        changed[arc_path] = arc.data
    for jpc_path, jpc in rando.jpcs_by_path.items():
        jpc.save()
        changed[jpc_path] = jpc.data

    def vanilla_bytes(path):
        if path not in vanilla_gcm.files_by_path:
            return None
        entry = vanilla_gcm.files_by_path[path]
        with open(vanilla_gcm.iso_path, "rb") as iso:  # read_file_data refuses the multi-MB videos
            iso.seek(entry.file_data_offset)
            return iso.read(entry.file_size)

    def vanilla_arc(path):
        data = vanilla_bytes(path)
        return None if data is None else RARC(io.BytesIO(data))

    def inner_files(arc: RARC):
        out = {}
        for e in arc.file_entries:
            if e.is_dir:
                continue
            e.decompress_data_if_necessary()
            out[e.name] = as_bytes(e.data)
        return out

    for path, f in sorted(changed.items()):
        data = as_bytes(f)
        if path == "files/RELS.arc":
            res.notes.append("files/RELS.arc is not written; its RELs are written as files/rels/NAME.rel")
            continue
        old = vanilla_bytes(path)
        if old is not None and Yaz0.check_is_compressed(io.BytesIO(old)):
            old = as_bytes(Yaz0.decompress(io.BytesIO(old)))  # compare and report decompressed (wwrando reads it so)
        if path.startswith("files/rels/"):
            old = None if old is None else prep_rel(old)
            data = prep_rel(data)
        row = {"path": path, "size": len(data), "sha1": sha1(data), "added": old is None,
               "vanilla_size": None if old is None else len(old), "vanilla_sha1": None if old is None else sha1(old)}
        if path in rando.arcs_by_path:
            row["kind"] = "arc"
            if old is None:
                row["inner_changed"] = None
            else:
                old_arc = RARC(io.BytesIO(old))
                a, b = inner_files(old_arc), inner_files(rando.arcs_by_path[path])
                ch = sorted(n for n in b if n in a and a[n] != b[n])
                add = sorted(n for n in b if n not in a)
                rem = sorted(n for n in a if n not in b)
                row["inner_changed"], row["inner_added"], row["inner_removed"] = ch, add, rem
                row["inner_sha1"] = {n: sha1(b[n]) for n in ch + add}
                if not (ch or add or rem):
                    continue  # re-packed only
        elif path == "sys/main.dol":
            row["kind"] = "dol"
            if old == data:
                continue
        elif path.startswith("files/rels/"):
            row["kind"] = "rel"
        else:
            row["kind"] = "raw"
            if old == data:
                continue
        res.files[path] = data
        res.rows.append(row)

    # RELs that live in RELS.arc
    old_arc = vanilla_arc("files/RELS.arc")
    for rel_path, data in sorted(rel_files.items()):
        if rel_path in changed:
            continue  # standalone disc file, handled above
        name = os.path.basename(rel_path)
        e = old_arc.get_file_entry(name)
        old = None
        if e is not None:
            e.decompress_data_if_necessary()
            old = prep_rel(as_bytes(e.data))
        if old is not None and old == data:
            continue
        res.files[rel_path] = data
        res.rows.append({"path": rel_path, "kind": "rel", "source": "RELS.arc", "size": len(data), "sha1": sha1(data),
                         "added": False, "vanilla_size": len(old), "vanilla_sha1": sha1(old)})
    return res


def parse_args(argv):
    ap = argparse.ArgumentParser(description="wwrando's patched game files without writing an ISO.")
    ap.add_argument("--iso", default=DEFAULT_ISO, help="vanilla GZLE01 disc (read only)")
    ap.add_argument("--out", required=True, help="output folder (created)")
    ap.add_argument("--seed")
    ap.add_argument("--permalink")
    ap.add_argument("--options", help="JSON file of wwrando option names to values")
    ap.add_argument("--set", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--iso-out", metavar="FILE",
                    help="instead of collecting changed files, let wwrando write its own complete randomized ISO to FILE "
                         "(and its spoiler and non-spoiler logs into --out); the per-seed build (scripts/windows/build.py "
                         "--rando-iso) is made from it")
    ap.add_argument("--files", choices=("all", "code", "none"), default="all",
                    help="which patched files to write: all, code (main.dol and the RELs only) or none (manifest only)")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    out = Path(args.out).resolve()
    t0 = time.perf_counter()
    try:
        from options.wwrando_options import Options
        import randomizer as rmod
        from randomizer import WWRandomizer
        from wwrando import make_argparser
        from seedgen import seedgen
        from gclib.gcm import GCM
    except Exception as e:
        gs.fail(gs.EXIT_RUNTIME, f"cannot import wwrando ({e}); run: python scripts/windows/rando_runtime.py --force")

    try:
        user_options = {}
        if args.options:
            user_options.update(json.loads(Path(args.options).read_text(encoding="utf-8")))
        user_options.update(gs.parse_set(args.set))
        bunny_hood, bunny_start, user_options, permalink = gs.take_bunny_hood(user_options, args.permalink)
        if permalink:
            if args.seed or user_options:
                raise ValueError("--permalink carries the seed and options; do not combine it with --seed, --options or --set")
            seed, options = WWRandomizer.decode_permalink(permalink)
        else:
            options = gs.build_options(Options, user_options)
            seed = args.seed if args.seed else seedgen.make_random_seed_name()
        gs.reject_unsupported(options)
        if not Path(args.iso).is_file():
            raise ValueError(f"disc not found: {args.iso}")
    except Exception as e:
        gs.fail(gs.EXIT_ARGS, f"{type(e).__name__}: {e}")

    options.dry_run = False
    if bunny_hood:
        gs.add_bunny_hood_to_pool()
    out.mkdir(parents=True, exist_ok=True)
    cmd = make_argparser().parse_args(args=[])
    # Release behaviour: wwrando sets the game's developer-mode byte when run from a source tree.
    rmod.IS_RUNNING_FROM_SOURCE = False

    install_quantizer()
    rando = None
    try:
        with contextlib.redirect_stdout(sys.stderr):
            rando = WWRandomizer(seed=seed, clean_iso_path=args.iso, randomized_output_folder=str(out), options=options,
                                 cmd_line_args=cmd)
            if bunny_hood:
                gs.register_bunny_hood(rando)
            if args.iso_out:
                rando.randomize_all()  # wwrando's own run: patched files, ISO, logs (in --out)
                made = out / f"WW Random {rando.seed}.iso"
                dst = Path(args.iso_out).resolve()
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(made), str(dst))  # (the work folder may be on another drive: os.replace would fail)
                print(json.dumps({"ok": True, "seed": rando.seed, "permalink": rando.permalink, "iso": str(dst),
                                  "out": str(out), "seconds": round(time.perf_counter() - t0, 1)}), flush=True)
                return 0
            # Stop before the GCM/ISO write: the "save" step becomes a no-op generator; the collector below does the rest.
            rando.save_randomized_iso = lambda: iter(())
            rando.write_spoiler_log = lambda: None
            rando.write_non_spoiler_log = lambda: None
            rando.randomize_all()
            vanilla = GCM(args.iso)
            vanilla.read_entire_disc()
            res = collect(rando, vanilla)
    except Exception as e:
        tb = traceback.format_exc()
        print(tb, file=sys.stderr)
        gs.fail(gs.EXIT_GENERATION, "patching failed: " + f"{type(e).__name__}: {e}".strip().splitlines()[0], seed=seed)

    # a clean output: drop the files of an earlier run (only the folders and manifest this script writes)
    for sub in ("files", "sys"):
        shutil.rmtree(out / sub, ignore_errors=True)
    if args.files != "none":
        for path, data in res.files.items():
            # "code": main.dol and the RELs, and the title logo archive (the file select's Randomizer quest shows it)
            if args.files == "code" and not (path == "sys/main.dol" or path.startswith("files/rels/") or
                                             path == "files/res/Object/TlogoE.arc"):
                continue
            dst = out / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
    manifest = {
        "schema": "bod.patchmanifest", "schema_version": 1,
        "seed": rando.seed, "permalink": rando.permalink, "wwrando_version": rmod.VERSION,
        "options": {o.name: str(options[o.name]) if not isinstance(options[o.name], (bool, int, list)) else options[o.name]
                    for o in type(options).all() if o.name != "dry_run"},
        "notes": res.notes,
        "counts": {k: sum(1 for r in res.rows if r["kind"] == k) for k in ("dol", "rel", "arc", "raw")},
        "files": res.rows,
    }
    (out / "patch_manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"ok": True, "seed": rando.seed, "out": str(out), "files": len(res.rows), "counts": manifest["counts"],
                      "seconds": round(time.perf_counter() - t0, 1)}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
