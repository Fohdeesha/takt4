"""Where a system's downbeats land in the reference bar — the error taxonomy of Hockman,
Davies & Fujinaga (ISMIR 2012, the HJDB paper), whose statistics 2, 3 and 4 count
downbeats found a whole beat, two beats or three beats off, and whose finding was that
general models put the downbeat on beats two and four (the snares) and confuse one with
three (breakbeat cells repeat every two beats).

    python tools/refeval/downbeat_offset.py takt4:nofold takt4:pf

For every downbeat a system called, the reference beat nearest to it within 70 ms, and
that beat's position in the reference bar. A downbeat F-measure says how many were right;
this says what the wrong ones were: a bar phase error (mostly on 3) is not the same fault
as a backbeat preference (mostly on 2 and 4), and neither is the same as downbeats that
are not on any beat at all.
"""
import sys

import numpy as np

from common import HARD_EIGHT, REFS, beats_path, load_beats, stems, system_dir


def offsets(ref_t, ref_l, sys_t, sys_l, tolerance=0.07, start=5.0):
    """{position: count} over the system's downbeats after `start`, plus 'off' for those
    not within `tolerance` of any reference beat."""
    end = ref_t[-1] - 5.0
    down = sys_t[(sys_l == 1) & (sys_t >= start) & (sys_t <= end)]
    counts = {}
    for t in down:
        k = int(np.argmin(np.abs(ref_t - t)))
        if abs(ref_t[k] - t) <= tolerance and ref_l[k] > 0:
            counts[int(ref_l[k])] = counts.get(int(ref_l[k]), 0) + 1
        else:
            counts["off"] = counts.get("off", 0) + 1
    return counts, len(down)


def main(argv):
    ref = "ref_beatthis"
    specs = [a for a in argv if not a.startswith("--")]
    systems = [system_dir(s) for s in specs]
    ref_dir = REFS / ref
    positions = (1, 2, 3, 4, "off")
    totals = {spec: {"all": {}, "agreed": {}, "hard": {}} for spec, _, _ in systems}
    print(f"{'track':<36} " + " | ".join(f"{spec[:20]:^28}" for spec, _, _ in systems))
    print(f"{'':<36} " + " | ".join(f"{'n':>4} {'1%':>4} {'2%':>4} {'3%':>4} {'4%':>4} {'off%':>4}"
                                    for _ in systems))
    ref2 = REFS / "ref_madmom"
    for stem in stems(ref):
        ref_t, ref_l, _ = load_beats(ref_dir / f"{stem}.beats")
        if len(ref_t) < 3 or not np.any(ref_l == 1):
            continue
        hard = stem in HARD_EIGHT
        cells = []
        for spec, d, tag in systems:
            p = beats_path(d, stem, tag)
            if not p.exists():
                cells.append(f"{'-':>28}")
                continue
            sys_t, sys_l, _ = load_beats(p)
            counts, n = offsets(ref_t, ref_l, sys_t, sys_l)
            for group in ("all", "hard" if hard else "agreed"):
                for k, v in counts.items():
                    totals[spec][group][k] = totals[spec][group].get(k, 0) + v
            pct = lambda k: 100.0 * counts.get(k, 0) / n if n else 0.0
            cells.append(f"{n:>4} " + " ".join(f"{pct(k):>4.0f}" for k in positions))
        print(f"{stem[:36]:<36} " + " | ".join(cells))
    print()
    for spec in totals:
        for group, label in (("all", "all 23"), ("agreed", "agreed 15"), ("hard", "hard 8")):
            c = totals[spec][group]
            n = sum(c.values())
            if not n:
                continue
            print(f"{spec:<16} {label:<10} n={n:5d}  " +
                  "  ".join(f"on {k}: {100.0 * c.get(k, 0) / n:4.1f}%" for k in positions))


if __name__ == "__main__":
    main(sys.argv[1:])
