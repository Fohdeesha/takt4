"""How steady the tracker's published tempo and meter are, from `takt4-cli track --trace`.

The accuracy gate in `tests/data/tracking/evaluation/` answers "is it right". This answers
"does it sit still", which is a different question and the one operators actually report
on. Nothing in mir_eval measures it: a tracker that publishes the correct tempo while
flickering an octave either side of it scores perfectly and is unusable.

It also needs *full tracks*. Ballroom's clips are thirty seconds, which is shorter than the
constants being tuned — `kMeterMemory` averages over about fifty beats — so most of what
those constants do happens after the clip has ended. Tuning them on excerpts is how a meter
that changed every thirteen seconds passed for settled; see that directory's README.

    C:/build/.../takt4-cli.exe track TRACK.wav --trace TRACK.trace
    python tools/trace_stability.py *.trace

The audio is the operator's own, so nothing here is committed and no file is assumed. Any
mono 22050 Hz WAV will do; `librosa.load(path, sr=22050, mono=True)` is how the reference
tracks were decoded.

The three counts, and what each one means when it moves:

  jumps        the published tempo changing by more than `--jump` from one 50 Hz frame to
               the next, split by what the lock was doing. **Read the split, not the
               total.** "Hunting" jumps before the first lock are acquisition and are
               honest — the tracker says it is hunting. "While locked" is the beat-spacing
               refinement wandering. "As the lock changed" is the tracker deciding the
               tempo is something else, which is the one an operator calls a fault.
  meter        the published beats-per-bar changing at all.
  unlocks      the lock being given up. Cheap on its own now — the published tempo is held
               through one — so this is a measure of how hard the material is rather than
               of how the tempo reads.
"""

import argparse
import sys
from pathlib import Path

import numpy as np

COLUMNS = ("time", "bpm", "meter", "locked")


def read(path):
    rows = np.genfromtxt(path, delimiter="\t", names=True)
    missing = [c for c in COLUMNS if c not in (rows.dtype.names or ())]
    if missing:
        raise SystemExit(f"{path}: not a --trace file (no {', '.join(missing)} column)")
    return rows


def summarise(rows, threshold):
    time, bpm, meter, locked = (rows[c] for c in COLUMNS)
    moved = np.abs(np.diff(bpm)) > threshold * np.maximum(bpm[:-1], 1e-9)
    at = np.flatnonzero(moved)
    before, after = locked[at], locked[at + 1]
    return {
        "seconds": float(time[-1] - time[0]) if len(time) > 1 else 0.0,
        "bpm": float(np.median(bpm[bpm > 0])) if np.any(bpm > 0) else 0.0,
        "jumps": int(at.size),
        "locked_jumps": int(np.sum((before == 1) & (after == 1))),
        "lock_change_jumps": int(np.sum(before != after)),
        "hunting_jumps": int(np.sum((before == 0) & (after == 0))),
        "meter_changes": int(np.sum(np.diff(meter) != 0)),
        "unlocks": int(np.sum(np.diff(locked) < 0)),
        "locked_fraction": float(np.mean(locked)),
    }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("traces", nargs="+", help="files written by `track --trace`")
    parser.add_argument("--jump", type=float, default=0.02,
                        help="fractional tempo change counted as a jump, default 0.02")
    options = parser.parse_args(argv)

    header = ("track", "sec", "bpm", "jumps", "lockd", "chng", "hunt", "meter", "unlk", "lock%")
    print(f"{header[0]:<40} {header[1]:>6} {header[2]:>7} {header[3]:>6} {header[4]:>6} "
          f"{header[5]:>5} {header[6]:>5} {header[7]:>6} {header[8]:>5} {header[9]:>6}")
    totals = {}
    for name in options.traces:
        path = Path(name)
        one = summarise(read(path), options.jump)
        for key, value in one.items():
            if key not in ("bpm", "locked_fraction"):
                totals[key] = totals.get(key, 0) + value
        print(f"{path.stem[:39]:<40} {one['seconds']:>6.0f} {one['bpm']:>7.1f} "
              f"{one['jumps']:>6} {one['locked_jumps']:>6} {one['lock_change_jumps']:>5} "
              f"{one['hunting_jumps']:>5} {one['meter_changes']:>6} {one['unlocks']:>5} "
              f"{one['locked_fraction'] * 100:>6.1f}")

    if not totals:
        return 0
    minutes = totals["seconds"] / 60.0
    print(f"\n{len(options.traces)} tracks, {minutes:.0f} minutes: "
          f"{totals['jumps']} tempo jumps over {options.jump:.0%} "
          f"({totals['locked_jumps']} while locked, {totals['lock_change_jumps']} as the "
          f"lock changed, {totals['hunting_jumps']} while hunting), "
          f"{totals['meter_changes']} meter changes, {totals['unlocks']} unlocks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
