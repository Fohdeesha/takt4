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
  intensity    §5.8's classifier changing state, and how much of the track it spent in
               each. Its constants have the same problem the meter's do — the long
               follower averages over twenty seconds, which is most of a Ballroom clip —
               so this is the only place they can honestly be chosen. A steady four-to-the
               -floor track that changes state every few seconds is flickering; one that
               never changes at all over a track with a breakdown in it is asleep.
  onsets       flux peaks per second, which is the number to sanity-check "on onset"
               against: a drum track is a few a second, and forty a second is noise.
  beat/m, x    the rate the tracker actually *published* beats at, and that over the tempo
               it printed. **This is the one to read first.** They are two claims and
               nothing used to make them agree: the octave fold moved the number and left
               the filter's beats alone, so "03 - Fake Sweat" published 92 BPM while firing
               125 beats a minute — x = 1.36 — and every OSC datagram, MIDI clock tick and
               trigger rule went out on the wrong grid under a readout that looked right.
               An x of 1.00 is the tracker agreeing with itself; 2.00 or 0.50 is an octave
               published but not played. See `TempoTracker::Options::foldBeats`.
"""

import argparse
import sys
from pathlib import Path

import numpy as np

COLUMNS = ("time", "bpm", "meter", "locked")
# Written since the intensity classifier; a trace from before it has neither.
OPTIONAL = ("intensity", "onset", "published")


def read(path):
    rows = np.genfromtxt(path, delimiter="\t", names=True)
    missing = [c for c in COLUMNS if c not in (rows.dtype.names or ())]
    if missing:
        raise SystemExit(f"{path}: not a --trace file (no {', '.join(missing)} column)")
    return rows


def summarise(rows, threshold):
    time, bpm, meter, locked = (rows[c] for c in COLUMNS)
    have = rows.dtype.names or ()
    seconds = float(time[-1] - time[0]) if len(time) > 1 else 0.0
    intensity = rows["intensity"] if "intensity" in have else np.ones_like(bpm)
    onset = rows["onset"] if "onset" in have else np.zeros_like(bpm)
    moved = np.abs(np.diff(bpm)) > threshold * np.maximum(bpm[:-1], 1e-9)
    at = np.flatnonzero(moved)
    before, after = locked[at], locked[at + 1]
    # What the beats actually came out at, against what the readout said they would. These
    # are two different claims and they were silently allowed to differ by a factor of two:
    # the octave fold moved the number and left `TrackedFrame::emitted` alone, so a track
    # published as 92 BPM fired 186 beats a minute at OSC, MIDI clock and every rule. See
    # `TempoTracker::Options::foldBeats`. A ratio near 1 is the tracker agreeing with itself.
    published = rows["published"] if "published" in have else None
    beats = int(np.sum(published != 0)) if published is not None else 0
    beat_bpm = beats / seconds * 60.0 if published is not None and seconds > 0 else 0.0
    median_bpm = float(np.median(bpm[bpm > 0])) if np.any(bpm > 0) else 0.0
    return {
        "seconds": seconds,
        "bpm": median_bpm,
        "beat_bpm": beat_bpm,
        "beat_ratio": beat_bpm / median_bpm if median_bpm > 0 and beat_bpm > 0 else 0.0,
        "beats": beats,
        "jumps": int(at.size),
        "locked_jumps": int(np.sum((before == 1) & (after == 1))),
        "lock_change_jumps": int(np.sum(before != after)),
        "hunting_jumps": int(np.sum((before == 0) & (after == 0))),
        "meter_changes": int(np.sum(np.diff(meter) != 0)),
        "unlocks": int(np.sum(np.diff(locked) < 0)),
        "locked_fraction": float(np.mean(locked)),
        "intensity_changes": int(np.sum(np.diff(intensity) != 0)),
        "onsets": int(np.sum(onset != 0)),
        "calm_fraction": float(np.mean(intensity == 0)),
        "intense_fraction": float(np.mean(intensity == 2)),
    }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("traces", nargs="+", help="files written by `track --trace`")
    parser.add_argument("--jump", type=float, default=0.02,
                        help="fractional tempo change counted as a jump, default 0.02")
    options = parser.parse_args(argv)

    header = ("track", "sec", "bpm", "beat/m", "x", "jumps", "lockd", "chng", "hunt", "meter",
              "unlk", "lock%", "int", "calm%", "int%", "ons/s")
    print(f"{header[0]:<34} {header[1]:>5} {header[2]:>6} {header[3]:>6} {header[4]:>5} "
          f"{header[5]:>6} {header[6]:>6} {header[7]:>5} {header[8]:>5} {header[9]:>5} "
          f"{header[10]:>5} {header[11]:>6} {header[12]:>4} {header[13]:>6} {header[14]:>5} "
          f"{header[15]:>6}")
    totals = {}
    worst = None
    for name in options.traces:
        path = Path(name)
        one = summarise(read(path), options.jump)
        for key, value in one.items():
            if not key.endswith("_fraction") and key not in ("bpm", "beat_bpm", "beat_ratio"):
                totals[key] = totals.get(key, 0) + value
        rate = one["onsets"] / one["seconds"] if one["seconds"] > 0 else 0.0
        if one["beat_ratio"] > 0 and (worst is None or abs(np.log2(one["beat_ratio"])) >
                                      abs(np.log2(worst[1]))):
            worst = (path.stem, one["beat_ratio"])
        print(f"{path.stem[:33]:<34} {one['seconds']:>5.0f} {one['bpm']:>6.1f} "
              f"{one['beat_bpm']:>6.1f} {one['beat_ratio']:>5.2f} "
              f"{one['jumps']:>6} {one['locked_jumps']:>6} {one['lock_change_jumps']:>5} "
              f"{one['hunting_jumps']:>5} {one['meter_changes']:>5} {one['unlocks']:>5} "
              f"{one['locked_fraction'] * 100:>6.1f} {one['intensity_changes']:>4} "
              f"{one['calm_fraction'] * 100:>6.1f} {one['intense_fraction'] * 100:>5.1f} "
              f"{rate:>6.2f}")

    if not totals:
        return 0
    minutes = totals["seconds"] / 60.0
    print(f"\n{len(options.traces)} tracks, {minutes:.0f} minutes: "
          f"{totals['jumps']} tempo jumps over {options.jump:.0%} "
          f"({totals['locked_jumps']} while locked, {totals['lock_change_jumps']} as the "
          f"lock changed, {totals['hunting_jumps']} while hunting), "
          f"{totals['meter_changes']} meter changes, {totals['unlocks']} unlocks")
    print(f"{totals['intensity_changes']} intensity changes "
          f"(one every {totals['seconds'] / max(1, totals['intensity_changes']):.0f} s), "
          f"{totals['onsets']} onsets ({totals['onsets'] / max(1e-9, totals['seconds']):.2f}/s)")
    if worst is not None:
        print(f"beats against the published tempo: worst ratio {worst[1]:.2f} on {worst[0]} "
              f"(1.00 is the tracker agreeing with itself; 2.00 or 0.50 is an octave "
              f"published but not played)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
