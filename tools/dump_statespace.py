#!/usr/bin/env python3
"""Precompute madmom's bar-pointer state space and transition models into one blob.

HANDOFF §5.4: `BarStateSpace`, `BarTransitionModel`, `TransitionModel` and
`ObservationModel` "depend only on configuration, never on audio", so the C++ tracker
does not have to reimplement any of madmom — it reads the finished tables and only ports
the runtime loop. This writes them.

    python tools/dump_statespace.py

Output is assets/statespace/default.bin plus a .json recording where it came from, in
the manner of tools/convert_weights.py. The defaults are BeatNet+'s, read out of
`particle_filtering_cascade.particle_filter_cascade` rather than recalled: 55-215 BPM
over 300 requested tempi at 50 fps, 2 to 4 beats per bar, lambda 60 for tempo changes
and 0.1 for meter changes, and "B56" observation models on both state spaces.

What the file holds, and why it is this small
---------------------------------------------
madmom builds the beat state space as 42 tempo intervals (14 to 55 frames per beat,
linearly spaced because 300 requested tempi is more than the 42 integer intervals the
range allows) laid end to end into 1449 states. Its transition model has 2722 entries,
but they have only two shapes:

  * from any state that is not the last of its interval, to the next state, probability 1
  * from the last state of an interval, to the first state of some interval, following
    the exponential tempo distribution

There are 1407 of the first kind and they carry no information beyond "+1". So only the
second kind is stored, as one sparse row per interval. The script reconstructs madmom's
full dense transition model from what it is about to write and refuses to write anything
if the two are not identical, which is what makes the compression safe.

The observation models collapse just as far. With observation_lambda "B56" the border is
1/56 = 0.017857, narrower than the 1/55 spacing of the widest interval, so the only
states inside it are the ones at position 0 exactly. Every state is therefore either a
beat state (madmom's pointer 2, the 42 first states) or a non-beat state (pointer 0);
pointer 1 never occurs. The pointers are written out anyway, as madmom computes them, so
the C++ never has to know that.

The layout, which src/core/tracking/state_space.cpp reads back
--------------------------------------------------------------
A header, then these sections back to back, no padding between them:

     1  beat interval per interval               uint32  x num_intervals
     2  beat first state per interval            uint32  x num_intervals
     3  beat last state per interval             uint32  x num_intervals
     4  beat interval per state                  uint32  x num_states
     5  beat position per state                  float64 x num_states
     6  beat observation pointer per state       uint32  x num_states
     7  tempo transition row offsets             uint32  x num_intervals + 1
     8  tempo transition destination intervals   uint32  x num_transitions
     9  tempo transition probabilities           float64 x num_transitions
    10  ... the same 1-6 for the downbeat state space ...
    16  meter transition matrix, row-major       float64 x num_meters^2
"""

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT_DIR = ROOT / "assets" / "statespace"

MAGIC = b"TAKT4SSP"
FORMAT_VERSION = 1

# magic, version, fps, min/max bpm, num_tempi, the two lambdas, the meter range, the two
# observation lambdas, the two state spaces' sizes, the stored transition count, the
# payload length, and an FNV-1a of the payload.
HEADER = struct.Struct("<8sIIddIddIIIIIIIIIII")

# BeatNet+'s configuration, from particle_filtering_cascade.particle_filter_cascade.
DEFAULTS = dict(fps=50, min_bpm=55.0, max_bpm=215.0, num_tempi=300,
                lambda_beat=60.0, lambda_down=0.1,
                min_beats_per_bar=2, max_beats_per_bar=4,
                observation_lambda_beat=56, observation_lambda_down=56)


def fnv1a32(data):
    """FNV-1a over the payload, as tools/convert_weights.py does for the weight blobs."""
    h = 0x811C9DC5
    for byte in memoryview(data).cast("B"):
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def observation_pointers(state_space, observation_lambda):
    """madmom's `ObservationModel` pointers for a "B<lambda>" model.

    Restated from BeatNet+'s BDObservationModel so that nothing here depends on
    importing a class whose module also wants matplotlib.
    """
    pointers = np.zeros(state_space.num_states, dtype=np.uint32)
    border = 1.0 / observation_lambda
    pointers[state_space.state_positions % 1 < border] = 1
    pointers[state_space.state_positions < border] = 2
    return pointers


def tempo_rows(dense, first_states, last_states):
    """The exponential last-state transitions, as sparse rows over the intervals.

    Returns (offsets, to_interval, probability). Row i holds every transition out of the
    last state of interval i, in the order madmom's dense model lists them.
    """
    to_states, prev_states, probabilities = dense
    first_of = {int(s): i for i, s in enumerate(first_states)}
    offsets = [0]
    to_interval = []
    probability = []
    for last in last_states:
        for k in np.flatnonzero(prev_states == last):
            destination = int(to_states[k])
            if destination not in first_of:
                raise SystemExit(f"state {last} transitions to {destination}, which is not "
                                 "the first state of an interval")
            to_interval.append(first_of[destination])
            probability.append(float(probabilities[k]))
        offsets.append(len(to_interval))
    return (np.array(offsets, dtype=np.uint32),
            np.array(to_interval, dtype=np.uint32),
            np.array(probability, dtype=np.float64))


def check_reconstructs(dense, num_states, first_states, last_states, rows):
    """Rebuild madmom's dense transition model from the rows and the "+1" rule.

    Every entry must come back identical, probabilities included, or the compression
    above has thrown something away.
    """
    offsets, to_interval, probability = rows
    rebuilt = {}
    firsts = set(int(s) for s in first_states)
    lasts = set(int(s) for s in last_states)
    for state in range(num_states):
        if state in lasts:
            continue
        if state + 1 in firsts or state + 1 >= num_states:
            raise SystemExit(f"state {state} is not a last state but has no successor")
        rebuilt[(state, state + 1)] = 1.0
    for i, last in enumerate(last_states):
        for k in range(offsets[i], offsets[i + 1]):
            rebuilt[(int(last), int(first_states[to_interval[k]]))] = probability[k]

    to_states, prev_states, probabilities = dense
    if len(to_states) != len(rebuilt):
        raise SystemExit(f"rebuilt {len(rebuilt)} transitions, madmom has {len(to_states)}")
    for to, prev, p in zip(to_states, prev_states, probabilities):
        key = (int(prev), int(to))
        if key not in rebuilt:
            raise SystemExit(f"transition {key} is in madmom's model but not the blob")
        if rebuilt[key] != float(p):
            raise SystemExit(f"transition {key}: blob has {rebuilt[key]!r}, madmom {float(p)!r}")


def meter_transitions(count, lambda_down):
    """BeatNet+'s downbeat transition matrix: stay put, or move to any other meter.

    Restated from particle_filter_cascade.__init__, which builds this one by hand rather
    than taking it from madmom.
    """
    matrix = np.zeros((count, count), dtype=np.float64)
    for i in range(count):
        for j in range(count):
            matrix[i, j] = 1.0 - lambda_down if i == j else lambda_down / (count - 1)
    return matrix


def section(array, dtype):
    """One section of the payload: little-endian, contiguous, no padding."""
    return np.ascontiguousarray(array, dtype=np.dtype(dtype).newbyteorder("<")).tobytes()


def build(config):
    from madmom.features.beats_hmm import BarStateSpace, BarTransitionModel
    from madmom.ml.hmm import TransitionModel

    fps = config["fps"]
    min_interval = 60.0 * fps / config["max_bpm"]
    max_interval = 60.0 * fps / config["min_bpm"]

    beat = BarStateSpace(1, min_interval, max_interval, config["num_tempi"])
    down = BarStateSpace(1, config["min_beats_per_bar"], config["max_beats_per_bar"],
                         config["max_beats_per_bar"] - config["min_beats_per_bar"] + 1)

    # Both spaces model one beat; BarStateSpace keeps its first and last states in a list
    # with one entry per beat of the bar.
    if beat.num_beats != 1 or down.num_beats != 1:
        raise SystemExit("both state spaces must be built with num_beats=1")
    beat_first, beat_last = beat.first_states[0], beat.last_states[0]
    down_first, down_last = down.first_states[0], down.last_states[0]

    # BarStateSpace keeps the per-state intervals only; an interval's own length is the
    # one its first state carries. They have to come out distinct and ascending, because
    # "the first state whose interval matches this one" is how the filter finds the phase
    # of the particle cloud, and that has to name exactly one interval.
    beat_intervals = beat.state_intervals[beat_first]
    down_intervals = down.state_intervals[down_first]
    for name, intervals in (("beat", beat_intervals), ("downbeat", down_intervals)):
        if len(np.unique(intervals)) != len(intervals) or not np.all(np.diff(intervals) > 0):
            raise SystemExit(f"{name} intervals are not distinct and ascending: {intervals}")

    tm = BarTransitionModel(beat, config["lambda_beat"])
    dense = TransitionModel.make_dense(tm.states, tm.pointers, tm.probabilities)
    rows = tempo_rows(dense, beat_first, beat_last)
    check_reconstructs(dense, beat.num_states, beat_first, beat_last, rows)

    beat_pointers = observation_pointers(beat, config["observation_lambda_beat"])
    down_pointers = observation_pointers(down, config["observation_lambda_down"])
    meter = meter_transitions(len(down_first), config["lambda_down"])

    payload = b"".join([
        section(beat_intervals, np.uint32),
        section(beat_first, np.uint32),
        section(beat_last, np.uint32),
        section(beat.state_intervals, np.uint32),
        section(beat.state_positions, np.float64),
        section(beat_pointers, np.uint32),
        section(rows[0], np.uint32),
        section(rows[1], np.uint32),
        section(rows[2], np.float64),
        section(down_intervals, np.uint32),
        section(down_first, np.uint32),
        section(down_last, np.uint32),
        section(down.state_intervals, np.uint32),
        section(down.state_positions, np.float64),
        section(down_pointers, np.uint32),
        section(meter, np.float64),
    ])

    header = HEADER.pack(
        MAGIC, FORMAT_VERSION, fps, config["min_bpm"], config["max_bpm"],
        config["num_tempi"], config["lambda_beat"], config["lambda_down"],
        config["min_beats_per_bar"], config["max_beats_per_bar"],
        config["observation_lambda_beat"], config["observation_lambda_down"],
        beat.num_states, len(beat_intervals), len(rows[1]),
        down.num_states, len(down_intervals),
        len(payload), fnv1a32(payload))

    stats = {
        "beat_states": int(beat.num_states),
        "beat_interval_range": [int(beat_intervals[0]), int(beat_intervals[-1])],
        "beat_interval_count": int(len(beat_intervals)),
        "beat_bpm_range": [round(60.0 * fps / float(beat_intervals[-1]), 3),
                           round(60.0 * fps / float(beat_intervals[0]), 3)],
        "beat_transitions_madmom": int(len(dense[0])),
        "beat_transitions_stored": int(len(rows[1])),
        "beat_pointer_counts": {str(int(v)): int(c) for v, c in
                                zip(*np.unique(beat_pointers, return_counts=True))},
        "down_states": int(down.num_states),
        "down_intervals": [int(v) for v in down_intervals],
        "payload_bytes": len(payload),
    }
    return header + payload, stats


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR,
                        help=f"default: {DEFAULT_OUT_DIR}")
    parser.add_argument("--name", default="default",
                        help="basename of the blob (default: default)")
    for key, value in DEFAULTS.items():
        parser.add_argument(f"--{key.replace('_', '-')}", type=type(value), default=value)
    args = parser.parse_args()

    config = {key: getattr(args, key) for key in DEFAULTS}
    data, stats = build(config)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    path = args.out_dir / f"{args.name}.bin"
    path.write_bytes(data)

    import madmom
    info = {
        "name": args.name,
        "format_version": FORMAT_VERSION,
        "config": config,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "madmom": madmom.__version__,
        "numpy": np.__version__,
        **stats,
    }
    (args.out_dir / f"{args.name}.json").write_text(json.dumps(info, indent=2) + "\n",
                                                    encoding="utf-8", newline="\n")
    print(f"{path.name}  {len(data)} bytes: {stats['beat_states']} beat states over "
          f"{stats['beat_interval_count']} intervals ({stats['beat_bpm_range'][0]}-"
          f"{stats['beat_bpm_range'][1]} BPM), {stats['beat_transitions_stored']} of "
          f"madmom's {stats['beat_transitions_madmom']} transitions stored, "
          f"{stats['down_states']} downbeat states")
    return 0


if __name__ == "__main__":
    sys.exit(main())
