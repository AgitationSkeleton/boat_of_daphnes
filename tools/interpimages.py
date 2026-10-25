#!/usr/bin/env python3
"""Finds the parts of the picture that jitter in a run of in-between frames, from the pictures themselves.

  SA2_INTERP_TEST=0.5 SA2_INTERP_STEPS=4 SA2_INTERP_JITTER=<log> SA2_INTERP_DUMP_LABELS=1,2 SA2_DUMP_DIR=<dir> SA2_DUMP_MAX=<n> ...
  (a scenario with `jitter NAME` lines: tools/scenarios/interp_jitter_*.txt)
  python tools/interpimages.py <dir> [--out <dir>]

The dir holds j<label>_<game frame>_<step><b|n>.rgba: every in-between step of each game frame, drawn without (b) and with (n)
the newer treatments. Between two steps a tile of the picture changes by some amount; when it moves smoothly that is the same
at every step of a game frame, when it is held for a few steps and then jumps (an effect not blended with the camera, a particle
paired with the wrong one) the changes pile up in one step. A tile's score is the largest step's change over the mean (1: even;
4 with 4 steps: all of it in one step). Prints, per label and variant, the share of the picture's movement in tiles scoring
over 2, and writes a heat map (red: jitter) next to the dir's first picture of the label.
"""
import argparse
import collections
import os
import re
import struct

import numpy as np
from PIL import Image

TILE = 16


def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"RGBA"
    w, h, fmt = struct.unpack_from("<3I", d, 4)
    a = np.frombuffer(d, dtype=np.uint8, count=w * h * 4, offset=16).reshape(h, w, 4)
    rgb = a[:, :, [2, 1, 0]] if fmt == 1 else a[:, :, :3]
    return rgb.astype(np.float32)


def tiles(diff):
    h, w = diff.shape[:2]
    h, w = h // TILE * TILE, w // TILE * TILE
    return diff[:h, :w].mean(axis=2).reshape(h // TILE, TILE, w // TILE, TILE).mean(axis=(1, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--out", default=None)
    ap.add_argument("--steps", type=int, default=4)
    ap.add_argument("--min-energy", type=float, default=0.5, help="mean abs change per step below which a tile counts as still")
    a = ap.parse_args()
    out = a.out or a.dir
    groups = collections.defaultdict(list)  # (label, variant) -> [(seq, step, path)]
    for n in os.listdir(a.dir):
        m = re.match(r"j(\d+)_(\d+)_(\d+)([bn])\.rgba$", n)
        if m:
            groups[(int(m[1]), m[4])].append((int(m[2]), int(m[3]), os.path.join(a.dir, n)))
    for (lab, var), items in sorted(groups.items()):
        items.sort()
        phase = collections.defaultdict(list)  # step phase -> [tile diffs]
        prev = None
        bad = 0
        imgs = [load(path) for _, _, path in items]
        black = [float((im.sum(axis=2) == 0).mean()) for im in imgs]
        base = float(np.median(black))
        for (seq, step, path), img, blk in zip(items, imgs, black):
            if blk > base + 0.2:  # a picture the renderer left half drawn (a test run's read-back): skipped
                bad += 1
                prev = None
                continue
            # (consecutive: the same game frame's previous step, or the last step of the frame before)
            if prev is not None and (prev[0], prev[1]) in ((seq, step - 1), (seq - 1, a.steps - 1)):
                phase[step].append(tiles(np.abs(img - prev[2])))
            prev = (seq, step, img)
        if len(phase) < a.steps:
            continue
        # per tile, per phase: mean change; per tile, per game frame the score would be noisier - use the means
        means = np.stack([np.mean(phase[p], axis=0) for p in range(a.steps)])  # steps x th x tw
        mean_all = means.mean(axis=0)
        score = means.max(axis=0) / np.maximum(mean_all, 1e-6)
        energetic = mean_all > a.min_energy
        share = (mean_all * (energetic & (score > 2))).sum() / max((mean_all * energetic).sum(), 1e-6)
        print("label %d %s: %d frames (%d half-drawn pictures skipped); tiles moving %d; share of the movement that jitters (score > 2): %.1f%%; tiles scoring over 1.5: %d; mean score %.2f" %
              (lab, "without" if var == "b" else "with", len(items) // a.steps, bad, int(energetic.sum()), 100 * share, int((energetic & (score > 1.5)).sum()),
               float((score * mean_all)[energetic].sum() / max(mean_all[energetic].sum(), 1e-6))))
        heat = np.clip((score - 1) / 3, 0, 1) * energetic
        im = Image.fromarray((heat * 255).astype(np.uint8)).resize((heat.shape[1] * 4, heat.shape[0] * 4), Image.NEAREST)
        im.save(os.path.join(out, "heat_%02d%s.png" % (lab, var)))


if __name__ == "__main__":
    main()
