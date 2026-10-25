#!/usr/bin/env python3
"""Lists what is not blended right in a run of in-between frames.

  SA2_INTERP_TEST=0.5 SA2_INTERP_STEPS=4 SA2_INTERP_JITTER=<log>  boat-of-daphnes.exe ...  (a scenario: tools/scenarios/interp_jitter_*.txt)
  python tools/interpjitter.py <log> [--top 20]

For each drawn thing (an object's model part, a particle emitter's draw, ...) the log has its mean screen position
(x/z, y/z) at every build: frame f at t = 0, .25, .5, .75 (t = 0 is the game frame before f, t = 1 is f itself). The
positions at t = 0 of frames f and f+1 are the game's own, so the builds between should lie on the line between them. The
error of a build is its distance from that line; a thing that was held at the newer frame (not blended) is off by 1-t of the
step (the list is ordered by the error in pixels of a 640 wide picture, about 500 per view unit), one blended against the wrong partner by more. The score of a thing is its mean error over its mean step (0: blended
exactly; ~.4: not blended at all); only intervals where its patch count did not change are used. The total weights each
thing by its step (how much the eye follows it).
"""
import argparse
import collections
import math


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--min-step", type=float, default=0.0004, help="mean step below which a thing counts as still")
    ap.add_argument("--max-step", type=float, default=0.3, help="a step above this is a jump (a cut, a new thing), not movement")
    a = ap.parse_args()
    names = {}
    series = collections.defaultdict(lambda: collections.defaultdict(dict))  # label -> key -> frame -> {t: (n, x, y)}
    for line in open(a.log):
        p = line.split()
        if len(p) < 7:
            continue
        names[p[3]] = p[7] if len(p) > 7 else ""
        series[int(p[0])][p[3]].setdefault(int(p[1]), {})[round(float(p[2]), 2)] = (int(p[4]), float(p[5]), float(p[6]))
    for lab in sorted(series):
        report(lab, series[lab], a, names)


def report(lab, things, a, names):
    rows = []
    for key, frames in things.items():
        errs, steps = [], []
        for f, ts in frames.items():
            nxt = frames.get(f + 1)
            if not nxt or 0.0 not in ts or 0.0 not in nxt:
                continue
            n0, x0, y0 = ts[0.0]
            n1, x1, y1 = nxt[0.0]
            if n0 != n1:
                continue
            step = math.hypot(x1 - x0, y1 - y0)
            if step > a.max_step:
                continue
            for t, (n, x, y) in ts.items():
                if t == 0.0:
                    continue
                if n != n0:
                    break
                errs.append(math.hypot(x - (x0 + (x1 - x0) * t), y - (y0 + (y1 - y0) * t)))
            else:
                steps.append(step)
        if len(steps) < 8:
            continue
        mean = sum(steps) / len(steps)
        if mean < a.min_step:
            continue
        # errs has 3 values per interval
        rows.append((sum(errs) / len(errs) / mean, mean, key, len(steps), sum(errs) / len(errs) * 500))
    rows.sort(key=lambda r: -r[4])
    w = sum(r[1] for r in rows)
    tot = sum(r[0] * r[1] for r in rows)
    bad = sum(r[1] for r in rows if r[0] > 0.1)
    print("[label %d] things moving: %d; weighted error %.3f; share of movement not blended right %.0f%%; things off by more than 1 px: %d (worst %.1f px)" % (lab, len(rows), tot / w if w else 0, 100 * bad / w if w else 0, sum(1 for r in rows if r[4] > 1), rows[0][4] if rows else 0))
    for sc, mean, key, n, px in rows[: a.top]:
        print("  %s %s error %.2f px (score %.2f, step %.4f) intervals %d" % (key, names.get(key, ""), px, sc, mean, n))


if __name__ == "__main__":
    main()
