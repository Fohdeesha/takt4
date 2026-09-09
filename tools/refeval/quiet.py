"""Recovery after quiet passages. A passage is 'quiet' when the 1 s RMS drops at least
`drop` dB below the track's median for at least `min_len` seconds. For each system, the beat
F-measure against Beat This! in the `after` seconds following the passage's end, and inside
the passage itself (does the tracker keep a usable grid through it?).

    python tools/refeval/quiet.py takt4:nofold dec:fwd100 dec:viterbi_down100
"""
import sys

import mir_eval
import numpy as np
import soundfile as sf

from common import AUDIO, REFS, beats_path, load_beats, stems, system_dir


def quiet_passages(y, sr, drop=10.0, min_len=4.0):
    n = int(sr)
    frames = len(y) // n
    rms = np.array([20 * np.log10(np.sqrt(np.mean(y[i * n:(i + 1) * n] ** 2)) + 1e-9)
                    for i in range(frames)])
    median = np.median(rms)
    quiet = rms < median - drop
    passages = []
    start = None
    for i, q in enumerate(quiet):
        if q and start is None:
            start = i
        if not q and start is not None:
            if i - start >= min_len:
                passages.append((start, i))
            start = None
    # ignore a quiet run that reaches the end of the track: nothing follows it
    return passages


def f_in(ref, sys_t, lo, hi):
    r = ref[(ref >= lo) & (ref < hi)]
    s = sys_t[(sys_t >= lo) & (sys_t < hi)]
    if len(r) < 2:
        return None
    if len(s) < 1:
        return 0.0
    return mir_eval.beat.f_measure(r, s)


def main(argv):
    after = 10.0
    systems = [system_dir(spec) for spec in argv]
    print(f"{'track':<34} {'passage':>11} | " + " | ".join(f"{s[0][:14]:^14}" for s in systems))
    print(f"{'':<34} {'':>11} | " + " | ".join(f"{'in':>6} {'after':>6}" for _ in systems))
    totals = {s[0]: {"in": [], "after": []} for s in systems}
    for stem in stems("ref_beatthis"):
        y, sr = sf.read(str(AUDIO / f"{stem}.wav"), dtype="float32")
        passages = quiet_passages(y, sr)
        if not passages:
            continue
        ref = load_beats(REFS / "ref_beatthis" / f"{stem}.beats")[0]
        for lo, hi in passages:
            if hi + after > len(y) / sr - 2:
                continue
            cells = []
            for spec, d, tag in systems:
                p = beats_path(d, stem, tag)
                if not p.exists():
                    cells.append(f"{'-':>13}")
                    continue
                s = load_beats(p)[0]
                fi = f_in(ref, s, lo, hi)
                fa = f_in(ref, s, hi, hi + after)
                if fi is not None:
                    totals[spec]["in"].append(fi)
                if fa is not None:
                    totals[spec]["after"].append(fa)
                cells.append(f"{(fi if fi is not None else float('nan')):6.2f} "
                             f"{(fa if fa is not None else float('nan')):6.2f}")
            print(f"{stem[:34]:<34} {lo:4d}-{hi:<4d}s   | " + " | ".join(cells))
    print()
    for spec in totals:
        t = totals[spec]
        if t["after"]:
            print(f"{spec:<16} passages {len(t['after']):2d}: mean F inside {np.mean(t['in']):.3f}, "
                  f"mean F in the {after:.0f} s after {np.mean(t['after']):.3f}")


if __name__ == "__main__":
    main(sys.argv[1:])
