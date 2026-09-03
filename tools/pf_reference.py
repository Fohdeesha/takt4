#!/usr/bin/env python3
"""BeatNet+'s particle filter cascade, restated, and the reference the C++ is gated on.

HANDOFF §8 Phase 4 wants "a parity harness like Phase 3's". Phase 3 could hold the C++
against PyTorch directly because the network is deterministic. This one cannot: the
tracker is a particle filter, and upstream's own output moves when only the RNG seed
changes. Measured over the 18 committed excerpts, eight seeds of
`particle_filtering_cascade` agree with each other on **57%** of beat frames exactly,
0.83 F1 within one 20 ms frame and 0.91 F1 at MIR's 70 ms tolerance. There is no
tolerance at which "the C++ reproduces upstream's beat times" is a meaningful statement.

So the gate is split, the way Phase 3 split `tools/beatnet_model.py` from the real
`.pt` files:

  * This module restates the filter with an RNG that is specified rather than inherited
    — xoshiro256++ over a splitmix64 seeding, twenty lines, written the same way in
    src/core/tracking/. The C++ then reproduces this **exactly**, frame by frame, and
    tests/tracking/ is a hard regression gate.
  * `--check-upstream` runs the real `particle_filtering_cascade` over the same
    activations across several seeds and refuses to write anything unless this
    restatement agrees with it as well as it agrees with itself. That is what keeps the
    restatement honest about being the same filter.

Both stages read the same tables: assets/statespace/default.bin, from
tools/dump_statespace.py, is what the C++ reads too.

    python tools/dump_statespace.py
    python tools/pf_reference.py

Output is tests/data/tracking/<excerpt>.npy, one row per frame, int32:

    column 0   the median particle state, before that frame's motion — the reference
               calls this `gathering`, and every decision below rests on it
    column 1   the mode of the downbeat particles, `down_max`
    column 2   what was emitted: 0 nothing, 1 downbeat, 2 beat
    column 3   the beat interval the median sits in, in frames

Columns 0 and 1 are the filter's whole state summarised; gating on them fails at the
first frame that diverges rather than only when a beat moves.

Where this deviates from upstream, and why
------------------------------------------
1. **The particle count stays fixed.** Upstream appends 7 fresh particles whenever the
   activation exceeds 0.8 and then calls `np.delete` without assigning the result, so
   the beat cloud grows forever: over the 10-second excerpts it goes from 1500 to about
   1850, which extrapolates to ~125,000 particles in an hour. The downbeat cloud has the
   same shape of bug and grows by 2 per strong downbeat. Both deletes pass
   `len(self.st.first_states)`, which is 1 — the length of a list holding one array —
   where the injection count is 7 and 3. This restatement injects and removes the same
   number, which is what the code plainly means and what a filter that has to run all
   night needs.
2. **Resampling normalises through the cumulative sum** rather than dividing by
   `np.sum` first. numpy sums pairwise and C++ does not, so a shared sequential
   cumulative sum is the only way both sides can agree bit for bit. Same arithmetic,
   different rounding in the last place.
3. **The removed particles are drawn over the whole cloud**, not over the first
   `particle_size` of it as upstream's `np.random.choice(self.particle_size, ...)` does.
4. Upstream keeps every beat it has ever emitted in `self.path` and returns the lot on
   every call. This keeps the last one, which is all the algorithm reads.

Nothing else differs. The thresholds, the order of the two stages, the injection
pattern, the gating and the emission rules are upstream's, read from
`references/beatnet-plus/src/BeatNetPlus/particle_filtering_cascade.py` at commit
bb90eb0a9065b101a4b4c4cb2b2061950266cb4b.
"""

import argparse
import hashlib
import json
import struct
import sys
import types
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BLOB = ROOT / "assets" / "statespace" / "default.bin"
ACTIVATIONS_DIR = ROOT / "tests" / "data" / "model" / "generic"
DEFAULT_OUT_DIR = ROOT / "tests" / "data" / "tracking"

MASK64 = (1 << 64) - 1


# ----------------------------------------------------------------------------- the RNG

class Xoshiro256pp:
    """xoshiro256++ seeded by splitmix64, as src/core/tracking/random.hpp implements it.

    A particle filter that cannot be replayed cannot be debugged (HANDOFF §5.4), and a
    filter whose randomness comes from whatever numpy happens to do this release cannot
    be reproduced in C++ at all. This is the whole source of randomness on both sides:
    same seed, same draws, in the same order.
    """

    def __init__(self, seed):
        state = seed & MASK64
        self.s = []
        for _ in range(4):
            state = (state + 0x9E3779B97F4A7C15) & MASK64
            z = state
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
            self.s.append(z ^ (z >> 31))

    def next_u64(self):
        s = self.s
        sum03 = (s[0] + s[3]) & MASK64
        result = (((sum03 << 23) | (sum03 >> 41)) + s[0]) & MASK64
        t = (s[1] << 17) & MASK64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = ((s[3] << 45) | (s[3] >> 19)) & MASK64
        return result

    def next_double(self):
        """Uniform in [0, 1), the top 53 bits. Exact in IEEE double on both sides."""
        return (self.next_u64() >> 11) * (2.0 ** -53)

    def bounded(self, n):
        """Uniform integer in [0, n), by masked rejection. n must be positive."""
        mask = 1
        while mask < n:
            mask = (mask << 1) | 1
        while True:
            value = self.next_u64() & mask
            if value < n:
                return value


# -------------------------------------------------------------- the state space blob

class StateSpace:
    """One half of the blob; the same fields src/core/tracking/state_space.hpp exposes."""

    def __init__(self, intervals, first_states, last_states, state_intervals,
                 state_positions, pointers):
        self.intervals = intervals
        self.first_states = first_states
        self.last_states = last_states
        self.state_intervals = state_intervals
        self.state_positions = state_positions
        self.pointers = pointers
        self.num_states = len(state_intervals)
        self.num_intervals = len(intervals)
        self.interval_index = [0] * self.num_states
        for i, first in enumerate(first_states):
            for s in range(first, last_states[i] + 1):
                self.interval_index[s] = i
        self.is_last = [False] * self.num_states
        for s in last_states:
            self.is_last[s] = True
        self.is_beat = [p == 2 for p in pointers]

    def phase_of(self, state):
        return state - self.first_states[self.interval_index[state]]


class StateSpaceModel:
    """assets/statespace/*.bin, read back. Mirrors src/core/tracking/state_space.cpp."""

    MAGIC = b"TAKT4SSP"
    HEADER = struct.Struct("<8sIIddIddIIIIIIIIIII")

    def __init__(self, path):
        data = Path(path).read_bytes()
        fields = self.HEADER.unpack_from(data, 0)
        if fields[0] != self.MAGIC:
            raise SystemExit(f"{path}: not a takt4 state space blob")
        (_, self.format_version, self.fps, self.min_bpm, self.max_bpm, self.num_tempi,
         self.lambda_beat, self.lambda_down, self.min_beats_per_bar, self.max_beats_per_bar,
         self.observation_lambda_beat, self.observation_lambda_down,
         beat_states, beat_intervals, beat_transitions, down_states, down_intervals,
         payload_bytes, _checksum) = fields
        if len(data) != self.HEADER.size + payload_bytes:
            raise SystemExit(f"{path}: payload is {len(data) - self.HEADER.size} bytes, "
                             f"the header says {payload_bytes}")
        self.path = Path(path)
        self.sha256 = hashlib.sha256(data).hexdigest()
        self.T = 1.0 / self.fps

        at = self.HEADER.size

        def take(count, dtype):
            nonlocal at
            values = np.frombuffer(data, dtype=dtype, count=count, offset=at)
            at += count * np.dtype(dtype).itemsize
            return values.tolist()

        def space(num_states, num_intervals):
            return StateSpace(take(num_intervals, "<u4"), take(num_intervals, "<u4"),
                              take(num_intervals, "<u4"), take(num_states, "<u4"),
                              take(num_states, "<f8"), take(num_states, "<u4"))

        self.beat = space(beat_states, beat_intervals)
        self.tempo_offsets = take(beat_intervals + 1, "<u4")
        self.tempo_destinations = take(beat_transitions, "<u4")
        self.tempo_probabilities = take(beat_transitions, "<f8")
        self.downbeat = space(down_states, down_intervals)
        flat = take(down_intervals * down_intervals, "<f8")
        self.meter_transitions = [flat[i * down_intervals:(i + 1) * down_intervals]
                                  for i in range(down_intervals)]
        if at != len(data):
            raise SystemExit(f"{path}: {len(data) - at} bytes left over after the sections")

    def tempo_row(self, interval):
        lo, hi = self.tempo_offsets[interval], self.tempo_offsets[interval + 1]
        return self.tempo_destinations[lo:hi], self.tempo_probabilities[lo:hi]


# ------------------------------------------------------------------------- the filter

class ParticleFilter:
    """The runtime loop of `particle_filter_cascade`, one activation frame at a time."""

    PARTICLE_SIZE = 1500
    DOWN_PARTICLE_SIZE = 250
    IG_THRESHOLD = 0.4      # information gate
    RESAMPLE_THRESHOLD = 0.1
    INJECT_THRESHOLD = 0.8  # beat stage
    DOWN_INJECT_THRESHOLD = 0.7
    EMIT_THRESHOLD = 0.4
    INJECT_STRIDE = 6       # every 6th tempo, from a random one of the first 4
    INJECT_PHASES = 4

    NOTHING, DOWNBEAT, BEAT = 0, 1, 2

    def __init__(self, model, seed=1, particle_size=PARTICLE_SIZE,
                 down_particle_size=DOWN_PARTICLE_SIZE):
        self.model = model
        self.rng = Xoshiro256pp(seed)
        self.T = model.T
        # Upstream's `np.arange(0, num_states - 1)` leaves out the very last state; kept.
        self.particles = sorted(self.rng.bounded(model.beat.num_states - 1)
                                for _ in range(particle_size))
        self.down_particles = sorted(self.rng.bounded(model.downbeat.num_states - 1)
                                     for _ in range(down_particle_size))
        # The clutter has to be within the first 70 ms of the beat, and no beat may
        # follow the last one within 40% of a beat period.
        self.gather_window = int(0.07 / self.T) + 1
        self.counter = -1
        self.last_time = 0.0
        self.last_kind = self.NOTHING
        self.down_max = self._mode(self.down_particles, model.downbeat.num_states)

    # -- the pieces, each of which the C++ mirrors exactly ---------------------------

    @staticmethod
    def _median_int(particles):
        ordered = sorted(particles)
        half = len(ordered) // 2
        if len(ordered) % 2:
            return ordered[half]
        return int((ordered[half - 1] + ordered[half]) / 2.0)

    @staticmethod
    def _mode(particles, num_states):
        counts = [0] * num_states
        for p in particles:
            counts[p] += 1
        best, at = -1, 0
        for s, c in enumerate(counts):
            if c > best:
                best, at = c, s
        return at

    def _move(self, particles, space, row_of):
        """One step along the state space; a particle at a last state draws a new one."""
        moved = []
        wrapped = []
        for p in particles:
            if space.is_last[p]:
                wrapped.append(p)
            else:
                moved.append(p + 1)
        for p in wrapped:
            destinations, probabilities = row_of(space.interval_index[p])
            u = self.rng.next_double()
            cumulative = 0.0
            chosen = destinations[-1]
            for k, probability in enumerate(probabilities):
                cumulative += probability
                if u < cumulative:
                    chosen = destinations[k]
                    break
            moved.append(space.first_states[chosen])
        return moved

    def _resample(self, particles, weight_of):
        """Systematic resampling, upstream's `universal_resample`.

        The cumulative sum is compared against `u * total` rather than the weights being
        divided by `np.sum` first: numpy sums pairwise, C++ does not, and a sequential
        cumulative sum is something both can agree on to the last bit.
        """
        count = len(particles)
        cumulative = []
        running = 0.0
        for p in particles:
            running += weight_of(p)
            cumulative.append(running)
        total = running
        if not (total > 0.0):
            return list(particles)  # every hypothesis is impossible; leave them be
        step = 1.0 / count
        out = []
        at = 0
        for j in range(count):
            target = (self.rng.next_double() * step + j * step) * total
            while at + 1 < count and cumulative[at] < target:
                at += 1
            out.append(particles[at])
        return out

    def _remove(self, particles, count):
        """Drop `count` distinct particles, chosen uniformly, to undo an injection."""
        dropped = set()
        while len(dropped) < count:
            dropped.add(self.rng.bounded(len(particles)))
        return [p for i, p in enumerate(particles) if i not in dropped]

    # -- one frame -------------------------------------------------------------------

    def process(self, beat_activation, downbeat_activation):
        """Returns (emitted kind, gathering, down_max, interval) for this frame."""
        model = self.model
        self.counter += 1
        now = self.counter * self.T

        # The information gate: anything below the threshold is flattened to a floor, so
        # a quiet passage neither moves the cloud nor emits.
        gated = max(beat_activation, downbeat_activation)
        if gated < self.IG_THRESHOLD:
            gated = 0.03

        gathering = self._median_int(self.particles)
        interval_index = model.beat.interval_index[gathering]
        interval = model.beat.intervals[interval_index]
        emitted = self.NOTHING

        if (model.beat.phase_of(gathering) < self.gather_window
                and now - self.last_time > 0.4 * self.T * interval):
            down = model.downbeat
            self.down_particles = self._move(self.down_particles, down,
                                             lambda m: (list(range(down.num_intervals)),
                                                        model.meter_transitions[m]))
            injected = 0
            if downbeat_activation > self.DOWN_INJECT_THRESHOLD:
                self.down_particles.extend(down.first_states)
                injected = len(down.first_states)
            # madmom's pointer 2 selects the downbeat density, 0 the plain beat one.
            weights = [downbeat_activation if down.is_beat[s] else beat_activation
                       for s in range(down.num_states)]
            self.down_particles = self._resample(self.down_particles, weights.__getitem__)
            if injected:
                self.down_particles = self._remove(self.down_particles, injected)
            self.down_max = self._mode(self.down_particles, down.num_states)

            if (down.is_beat[self.down_max] and self.last_kind != self.DOWNBEAT
                    and downbeat_activation > self.EMIT_THRESHOLD):
                emitted = self.DOWNBEAT
            elif gated > self.EMIT_THRESHOLD:
                emitted = self.BEAT
            if emitted != self.NOTHING:
                self.last_time = now
                self.last_kind = emitted

        beat = model.beat
        self.particles = self._move(self.particles, beat, model.tempo_row)
        if gated > self.RESAMPLE_THRESHOLD:
            injected = 0
            if gated > self.INJECT_THRESHOLD:
                start = self.rng.bounded(self.INJECT_PHASES)
                fresh = [beat.first_states[i]
                         for i in range(start, beat.num_intervals, self.INJECT_STRIDE)]
                self.particles.extend(fresh)
                injected = len(fresh)
            self.particles = self._resample(
                self.particles, lambda p: gated if beat.is_beat[p] else 0.03)
            if injected:
                self.particles = self._remove(self.particles, injected)

        return emitted, gathering, self.down_max, interval


def run(model, activations, seed=1):
    """(frames, 4) int32: gathering, down_max, emitted kind, interval."""
    pf = ParticleFilter(model, seed=seed)
    out = np.empty((len(activations), 4), dtype=np.int32)
    for i, (beat, downbeat) in enumerate(activations):
        emitted, gathering, down_max, interval = pf.process(float(beat), float(downbeat))
        out[i] = (gathering, down_max, emitted, interval)
    return out


def beat_times(trace, seconds_per_frame):
    """The (time, kind) pairs upstream's `process` returns, out of a trace."""
    frames = np.flatnonzero(trace[:, 2] != 0)
    return np.column_stack((frames * seconds_per_frame, trace[frames, 2]))


# --------------------------------------------------- holding it against the real thing

def f_measure(reference, estimate, tolerance):
    """Beat-tracking F1: one-to-one nearest matches within `tolerance` seconds."""
    if len(reference) == 0 and len(estimate) == 0:
        return 1.0
    if len(reference) == 0 or len(estimate) == 0:
        return 0.0
    used = np.zeros(len(estimate), bool)
    hits = 0
    for r in reference:
        distance = np.abs(estimate - r)
        distance[used] = np.inf
        j = int(np.argmin(distance))
        if distance[j] <= tolerance:
            used[j] = True
            hits += 1
    precision, recall = hits / len(estimate), hits / len(reference)
    return 0.0 if precision + recall == 0 else 2 * precision * recall / (precision + recall)


#: What each side is summarised by, and how far below upstream's agreement with itself
#: the restatement is allowed to fall on each. See tests/data/tracking/README.md for
#: what these do and do not catch — measured, not assumed.
STATISTICS = ("beat_f1", "downbeat_f1", "same_tempo")
AGREEMENT_MARGIN = 0.05


def upstream_filter(activations, seed, upstream_dir):
    """Upstream's `particle_filter_cascade` over one excerpt, frame by frame.

    Returns the same three summaries `summarise` makes of this restatement: beat times,
    downbeat times, and the tempo interval the particle cloud sat on each frame. The
    last is read out of the filter's own state — `state_intervals[median(particles)]`,
    before the frame's motion, which is where upstream takes it too.

    Two shims, neither of which touches the algorithm: matplotlib is imported at module
    scope but only used when `plot` is non-empty, and numpy 2 removed `np.in1d`, which
    for the 1-D arrays upstream passes it is exactly `np.isin`.
    """
    if "matplotlib" not in sys.modules:
        mpl = types.ModuleType("matplotlib")
        mpl.pyplot = types.ModuleType("matplotlib.pyplot")
        sys.modules["matplotlib"] = mpl
        sys.modules["matplotlib.pyplot"] = mpl.pyplot
    if not hasattr(np, "in1d"):
        np.in1d = np.isin
    src = str(Path(upstream_dir) / "src")
    if src not in sys.path:
        sys.path.insert(0, src)
    from BeatNetPlus.particle_filtering_cascade import particle_filter_cascade

    np.random.seed(seed)
    estimator = particle_filter_cascade(beats_per_bar=[], fps=50, plot=[], mode="online")
    intervals = np.empty(len(activations), dtype=np.int32)
    for i, frame in enumerate(activations):
        intervals[i] = estimator.st.state_intervals[int(np.median(estimator.particles))]
        estimator.process(np.asarray(frame, dtype=np.float64).reshape(1, 2))
    path = estimator.path[1:]
    return path[:, 0], path[path[:, 1] == 1][:, 0], intervals


def summarise(trace, seconds_per_frame):
    """The same three summaries, out of one of this module's traces."""
    times = beat_times(trace, seconds_per_frame)
    return times[:, 0], times[times[:, 1] == ParticleFilter.DOWNBEAT][:, 0], trace[:, 3]


def agreement(a, b, tolerance):
    """How alike two runs are: beat F1, downbeat F1, and same-tempo frames."""
    return (f_measure(a[0], b[0], tolerance), f_measure(a[1], b[1], tolerance),
            float(np.mean(a[2] == b[2])))


def check_upstream(model, excerpts, upstream_dir, seeds, tolerance):
    """Refuse to write unless the restatement tracks upstream as well as upstream does.

    Upstream against itself over `seeds` different seeds is the yardstick, because a
    particle filter has no exact answer to be held to: eight seeds of upstream agree
    with each other on 91% of beats at MIR's 70 ms tolerance and no better. This
    restatement, over the same seeds, has to reach that band on all three summaries.
    """
    self_scores, cross_scores = [], []
    for name, activations in excerpts:
        upstream = [upstream_filter(activations, s, upstream_dir) for s in seeds]
        ours = [summarise(run(model, activations, seed=s), model.T) for s in seeds]
        mine, theirs = [], []
        for i in range(len(seeds)):
            for j in range(len(seeds)):
                if i < j:
                    theirs.append(agreement(upstream[i], upstream[j], tolerance))
                mine.append(agreement(upstream[i], ours[j], tolerance))
        self_scores += theirs
        cross_scores += mine
        print("  %-24s upstream vs itself %s   ours vs upstream %s" % (
            name, " ".join(f"{v:.3f}" for v in np.mean(theirs, axis=0)),
            " ".join(f"{v:.3f}" for v in np.mean(mine, axis=0))))
    return (dict(zip(STATISTICS, np.mean(self_scores, axis=0))),
            dict(zip(STATISTICS, np.mean(cross_scores, axis=0))))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--blob", type=Path, default=DEFAULT_BLOB, help=f"default: {DEFAULT_BLOB}")
    parser.add_argument("--activations", type=Path, default=ACTIVATIONS_DIR,
                        help=f"directory of (frames, 6) model traces; default: {ACTIVATIONS_DIR}")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR,
                        help=f"default: {DEFAULT_OUT_DIR}")
    parser.add_argument("--seed", type=int, default=1, help="the committed trace's seed")
    parser.add_argument("--check-upstream", type=Path,
                        default=ROOT / "references" / "beatnet-plus",
                        help="BeatNet+ checkout to compare against; '-' to skip")
    parser.add_argument("--check-seeds", type=int, default=6,
                        help="how many seeds each side is run with for that comparison")
    parser.add_argument("--check-tolerance", type=float, default=0.07,
                        help="MIR's beat tolerance, in seconds")
    args = parser.parse_args()

    model = StateSpaceModel(args.blob)
    paths = sorted(Path(args.activations).glob("*.npy"))
    if not paths:
        raise SystemExit(f"no activations in {args.activations}")
    excerpts = []
    for path in paths:
        trace = np.load(path)
        if trace.ndim != 2 or trace.shape[1] != 6:
            raise SystemExit(f"{path}: expected (frames, 6) from tools/model_reference.py")
        # Columns 3 and 4 are the softmax probabilities for beat and downbeat.
        excerpts.append((path.stem, trace[:, 3:5].astype(np.float64)))

    measured = None
    if str(args.check_upstream) != "-":
        if not (args.check_upstream / "src" / "BeatNetPlus").is_dir():
            raise SystemExit(f"{args.check_upstream} is not a BeatNet+ checkout; see "
                             "tools/convert_weights.py for where to get one, or pass "
                             "--check-upstream - to skip the comparison")
        seeds = list(range(1, args.check_seeds + 1))
        print(f"holding the restatement against upstream over {len(seeds)} seeds, on "
              f"{', '.join(STATISTICS)}:")
        theirs, ours = check_upstream(model, excerpts, args.check_upstream, seeds,
                                      args.check_tolerance)
        # Upstream is this far from itself; the restatement has to be no further. The
        # margin is for the fact that the two consume different RNG streams entirely.
        short = []
        for statistic in STATISTICS:
            floor = theirs[statistic] - AGREEMENT_MARGIN
            print(f"  {statistic:<12} upstream vs itself {theirs[statistic]:.4f}, "
                  f"ours vs upstream {ours[statistic]:.4f} (floor {floor:.4f})")
            if ours[statistic] < floor:
                short.append(statistic)
        if short:
            raise SystemExit("the restatement does not track upstream as well as upstream "
                             "tracks itself on " + ", ".join(short) + "; nothing written")
        measured = {
            "seeds": seeds,
            "tolerance_seconds": args.check_tolerance,
            "margin": AGREEMENT_MARGIN,
            "upstream_vs_itself": {k: round(float(v), 4) for k, v in theirs.items()},
            "restatement_vs_upstream": {k: round(float(v), 4) for k, v in ours.items()},
        }

    args.out_dir.mkdir(parents=True, exist_ok=True)
    traces = {}
    for name, activations in excerpts:
        trace = run(model, activations, seed=args.seed)
        np.save(args.out_dir / f"{name}.npy", trace)
        emitted = trace[:, 2]
        traces[name] = {
            "frames": int(trace.shape[0]),
            "sha256": hashlib.sha256((args.out_dir / f"{name}.npy").read_bytes()).hexdigest(),
            "beats": int((emitted != 0).sum()),
            "downbeats": int((emitted == ParticleFilter.DOWNBEAT).sum()),
            "median_interval": int(np.median(trace[:, 3])),
        }
        print(f"{name:<24} {traces[name]['beats']:3d} beats "
              f"({traces[name]['downbeats']:2d} downbeats), median interval "
              f"{traces[name]['median_interval']} frames")

    info = {
        "seed": args.seed,
        "rng": "xoshiro256++ seeded by splitmix64",
        "state_space": args.blob.name,
        "state_space_sha256": model.sha256,
        "input": "tests/data/model/generic/<name>.npy, columns 3 and 4",
        "columns": "0 gathering, 1 down_max, 2 emitted (0 none, 1 downbeat, 2 beat), "
                   "3 beat interval in frames",
        "particle_size": ParticleFilter.PARTICLE_SIZE,
        "down_particle_size": ParticleFilter.DOWN_PARTICLE_SIZE,
        "numpy": np.__version__,
        "agreement_with_upstream": measured,
        "excerpts": traces,
    }
    (args.out_dir / "reference.json").write_text(json.dumps(info, indent=2) + "\n",
                                                 encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
