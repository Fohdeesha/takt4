"""Per-window view of one track: where the tempo is right and the beats are not.

    python tools/refeval/sections.py 09_Defang [window seconds] [--tag nofold|fold70]

Columns per window: signal level (dBFS RMS), Beat This! local tempo and beats-per-bar,
madmom's local tempo, takt4's published tempo, lock fraction, confidence, takt4 beat F vs
Beat This! inside the window, the activation on/off ratio inside the window, and — where
decoders.py has run — each alternative decoder's local tempo and F.
"""
import argparse
import sys

import mir_eval
import numpy as np
import soundfile as sf

from common import AUDIO, DEC, FPS, REFS, TAKT4, load_beats, load_trace


def local_tempo(times, lo, hi):
    t = times[(times >= lo) & (times < hi)]
    if len(t) < 3:
        return 0.0
    return 60.0 / np.median(np.diff(t))


def bar_len(times, labels, lo, hi):
    m = (times >= lo) & (times < hi) & (labels == 1)
    d = np.flatnonzero(m)
    if len(d) < 2:
        return 0
    return int(np.median(np.diff(d)))


def act_ratio(act, ref_t, lo, hi):
    t = ref_t[(ref_t >= lo) & (ref_t < hi)]
    if len(t) < 3:
        return 0.0, 0.0
    mids = t[:-1] + np.diff(t) / 2

    def at(times):
        idx = np.round(times * FPS).astype(int)
        idx = idx[(idx > 0) & (idx < len(act) - 1)]
        return (float(np.stack([act[idx - 1], act[idx], act[idx + 1]], 1).max(1).mean())
                if len(idx) else 0.0)

    return at(t), at(mids)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stem")
    ap.add_argument("window", nargs="?", type=float, default=10.0)
    ap.add_argument("--tag", default="nofold")
    a = ap.parse_args(argv)
    stem, win = a.stem, a.window
    y, sr = sf.read(str(AUDIO / f"{stem}.wav"), dtype="float32")
    bt_t, bt_l, _ = load_beats(REFS / "ref_beatthis" / f"{stem}.beats")
    mm_t, mm_l, _ = load_beats(REFS / "ref_madmom" / f"{stem}.beats")
    tk_t, tk_l, _ = load_beats(TAKT4 / f"{stem}.{a.tag}.beats")
    rows = load_trace(stem, a.tag)
    act = rows["beat_act"] + rows["down_act"]
    extra = {}
    for tag in ("fwd", "fwd100", "viterbi_down", "viterbi_down100"):
        p = DEC / f"{stem}.{tag}.beats"
        if p.exists():
            extra[tag] = load_beats(p)[:2]
    dur = len(y) / sr
    print(f"{stem} ({a.tag}): {dur:.0f} s; Beat This! {len(bt_t)} beats, madmom {len(mm_t)}, "
          f"takt4 {len(tk_t)}")
    hdr = (f"{'t':>5} {'dBFS':>5} | {'BT bpm':>6} {'bar':>3} {'mm bpm':>6} | {'takt4':>6} "
           f"{'lock':>4} {'conf':>4} {'F':>5} {'on':>4} {'mid':>4} |")
    for tag in extra:
        hdr += f" {tag[:8]:>8}"
    print(hdr)
    for lo in np.arange(0, dur, win):
        hi = lo + win
        seg = y[int(lo * sr):int(hi * sr)]
        rms = 20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-9)
        f = (rows["time"] >= lo) & (rows["time"] < hi)
        bpm = float(np.median(rows["bpm"][f])) if f.any() else 0
        lock = float(np.mean(rows["locked"][f])) if f.any() else 0
        conf = float(np.mean(rows["confidence"][f])) if f.any() else 0
        r = bt_t[(bt_t >= lo) & (bt_t < hi)]
        s = tk_t[(tk_t >= lo) & (tk_t < hi)]
        F = mir_eval.beat.f_measure(r, s) if len(r) > 1 and len(s) > 1 else 0.0
        on, mid = act_ratio(act, bt_t, lo, hi)
        line = (f"{lo:5.0f} {rms:5.1f} | {local_tempo(bt_t, lo, hi):6.1f} "
                f"{bar_len(bt_t, bt_l, lo, hi):3d} {local_tempo(mm_t, lo, hi):6.1f} | "
                f"{bpm:6.1f} {lock:4.2f} {conf:4.2f} {F:5.2f} {on:4.2f} {mid:4.2f} |")
        for tag, (et, el) in extra.items():
            e = et[(et >= lo) & (et < hi)]
            Fe = mir_eval.beat.f_measure(r, e) if len(r) > 1 and len(e) > 1 else 0.0
            line += f" {local_tempo(et, lo, hi):4.0f}/{Fe:3.2f}"
        print(line)


if __name__ == "__main__":
    main(sys.argv[1:])
