"""Where takt4's beats fall relative to a reference grid, and whether the *activations*
already know where the beats are.

    python tools/refeval/phase.py [ref_beatthis|ref_madmom] [stem substrings...]

For each track:
  phase histogram   position of each takt4 beat inside the nearest reference beat period,
                    0 = on the beat, 0.5 = exactly between two reference beats.
  act on/off        mean P(beat of any kind) on reference beat frames against the frames
                    halfway between them. A ratio near 1 means the network does not see the
                    beat; a ratio well above 1 with bad tracking means the decoder lost it.
  act octave        the same ratio measured on the *half-period* grid (every reference beat
                    plus the midpoints): if the network fires there too, the double-time grid
                    is genuinely in the activations.
"""
import sys

import numpy as np

from common import FPS, REFS, TAKT4, load_beats, load_trace, stems


def activation(stem):
    rows = load_trace(stem, "nofold")
    return rows["beat_act"] + rows["down_act"], rows["bpm"], rows["locked"]


def mean_at(act, times):
    idx = np.round(times * FPS).astype(int)
    idx = idx[(idx >= 0) & (idx < len(act))]
    # the network's peak may be a frame either side, so take the max over +-1 frame
    win = np.stack([act[np.clip(idx + d, 0, len(act) - 1)] for d in (-1, 0, 1)], axis=1).max(1)
    return float(win.mean()) if len(win) else 0.0


def main(argv):
    ref = argv[0] if argv else "ref_beatthis"
    ref_dir = REFS / ref
    names = argv[1:]
    which = [s for s in stems(ref) if not names or any(n in s for n in names)]
    print(f"{'track':<40} {'refBPM':>6} {'on':>5} {'mid':>5} {'ratio':>5} {'ratio/2':>7} "
          f"| takt4 beat phase histogram (10 bins, 0=on beat)")
    for stem in which:
        ref_t, ref_l, _ = load_beats(ref_dir / f"{stem}.beats")
        act, bpm, locked = activation(stem)
        ref_t = ref_t[(ref_t > 5) & (ref_t < ref_t[-1] - 5)]
        if len(ref_t) < 4:
            continue
        period = np.median(np.diff(ref_t))
        mids = ref_t[:-1] + np.diff(ref_t) / 2
        on = mean_at(act, ref_t)
        mid = mean_at(act, mids)
        # half-period grid: every quarter is a candidate "beat" of the double-time grid
        q1 = ref_t[:-1] + np.diff(ref_t) / 4
        q3 = ref_t[:-1] + 3 * np.diff(ref_t) / 4
        quarter = mean_at(act, np.concatenate([q1, q3]))
        half_grid = (on + mid) / 2
        ratio2 = half_grid / quarter if quarter > 0 else 0
        sys_t, _, _ = load_beats(TAKT4 / f"{stem}.nofold.beats")
        sys_t = sys_t[(sys_t > 5) & (sys_t < ref_t[-1])]
        # phase of each takt4 beat inside the reference period that contains it
        k = np.searchsorted(ref_t, sys_t) - 1
        k = np.clip(k, 0, len(ref_t) - 2)
        phase = (sys_t - ref_t[k]) / (ref_t[k + 1] - ref_t[k])
        phase = np.where(phase > 0.95, phase - 1.0, phase)  # -0.05..0.95, so "on" is one bin
        hist, _ = np.histogram(phase, bins=10, range=(-0.05, 0.95))
        hist = np.round(100 * hist / max(1, hist.sum())).astype(int)
        print(f"{stem[:40]:<40} {60 / period:6.1f} {on:5.2f} {mid:5.2f} {on / max(mid, 1e-6):5.2f} "
              f"{ratio2:7.2f} | " + " ".join(f"{h:3d}" for h in hist))


if __name__ == "__main__":
    main(sys.argv[1:])
