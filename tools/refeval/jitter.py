"""Beat timing precision: signed offset of each system's beats from the nearest reference
beat, on tracks the system tracks well (F >= 0.9). mir_eval's 70 ms tolerance hides a
tracker that is 40 ms late on every beat; a lighting rig does not.

    python tools/refeval/jitter.py takt4:nofold dec:fwd100 dec:viterbi_down100 ref_madmom

This is an exit criterion for any decoder replacing the
particle filter: its 10-90 % spread and its fraction of beats more than 40 ms out may be
no worse than the PF's. `--json FILE` writes the per-system totals for gate.py.
"""
import argparse
import json
import sys

import mir_eval
import numpy as np

from common import REFS, beats_path, load_beats, stems, system_dir


def offsets_for(spec, ref="ref_beatthis", min_f=0.9):
    _, base, tag = system_dir(spec)
    ref_dir = REFS / ref
    offsets_all = []
    per_track = []
    for stem in stems(ref):
        p = beats_path(base, stem, tag)
        if not p.exists():
            continue
        ref_t = load_beats(ref_dir / f"{stem}.beats")[0]
        sys_t = load_beats(p)[0]
        ref_t = ref_t[(ref_t > 5) & (ref_t < ref_t[-1] - 5)]
        sys_t = sys_t[(sys_t > 5) & (sys_t < ref_t[-1])]
        if len(sys_t) < 3:
            continue
        F = mir_eval.beat.f_measure(ref_t, sys_t)
        if F < min_f:
            continue
        k = np.clip(np.searchsorted(ref_t, sys_t), 1, len(ref_t) - 1)
        nearest = np.where(np.abs(ref_t[k] - sys_t) < np.abs(ref_t[k - 1] - sys_t),
                           ref_t[k], ref_t[k - 1])
        off = (sys_t - nearest) * 1000
        off = off[np.abs(off) <= 70]
        offsets_all.append(off)
        per_track.append((stem, F, float(np.median(off)),
                          float(np.percentile(off, 90) - np.percentile(off, 10)),
                          float(np.mean(np.abs(off) > 40))))
    if not offsets_all:
        return None, per_track
    allo = np.concatenate(offsets_all)
    total = {"tracks": len(per_track), "median_ms": float(np.median(allo)),
             "spread_ms": float(np.percentile(allo, 90) - np.percentile(allo, 10)),
             "over_40ms": float(np.mean(np.abs(allo) > 40))}
    return total, per_track


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("systems", nargs="+")
    ap.add_argument("--ref", default="ref_beatthis")
    ap.add_argument("--json", default=None)
    a = ap.parse_args(argv)
    results = {}
    for spec in a.systems:
        total, per_track = offsets_for(spec, a.ref)
        if total is None:
            print(f"{spec}: no track above F 0.9")
            continue
        results[spec] = total
        print(f"== {spec}: {total['tracks']} tracks with F>=0.9; offset median "
              f"{total['median_ms']:+.0f} ms, 10-90% spread {total['spread_ms']:.0f} ms, "
              f"|off|>40 ms on {100 * total['over_40ms']:.1f}% of beats")
        for stem, F, med, spread, late in per_track:
            print(f"   {stem[:40]:<40} F {F:.3f}  median {med:+5.0f} ms  spread {spread:4.0f} ms"
                  f"  >40ms {100 * late:4.1f}%")
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(results, f, indent=2, sort_keys=True)
            f.write("\n")


if __name__ == "__main__":
    main(sys.argv[1:])
