"""Alternative decoders over takt4's *own* activations (beat_act, down_act from --trace).

Variants written to dec/<stem>.<variant>.beats ('<seconds>\\t<beat in bar>'):

  viterbi_down   madmom DBNDownBeatTrackingProcessor, offline Viterbi, meters [3, 4].
                 The ceiling for these activations: what a perfect decoder gets.
  viterbi_beat   madmom DBNBeatTrackingProcessor, offline, beat-only, on P(beat of any kind).
  online_beat    madmom's own online forward decoder (DBNBeatTrackingProcessor(online=True)).
  fwd            numpy forward filter over the joint (bar position x tempo x meter) HMM,
                 causal, beats emitted when the MAP state crosses a beat boundary.
  fwd_lagN       the same with N frames of fixed-lag smoothing (N x 20 ms of extra latency).

Per-frame tempo of `fwd` goes to dec/<stem>.fwd.tempo.npy for the stability comparison.
"""
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np
import scipy.sparse as sp

from common import DEC, FPS, TAKT4, load_trace, write_beats

TRACES = TAKT4
OUT = DEC


def load_one(stem, tag):
    rows = load_trace(stem, tag)
    return np.stack([rows["beat_act"], rows["down_act"]], axis=1).astype(np.float64)


def load_activations(stem, tag="nofold"):
    if tag in ("ens", "ens2"):
        members = ("nofold", "gmain", "afnp") if tag == "ens" else ("nofold", "gmain")
        parts = [load_one(stem, t) for t in members]
        n = min(len(x) for x in parts)
        act = np.mean([x[:n] for x in parts], axis=0)
    else:
        act = load_one(stem, tag)
    # A softmax over three classes: the two can sum to at most 1. Guard the rounding.
    s = act.sum(1)
    over = s > 0.999
    act[over] *= (0.999 / s[over])[:, None]
    return act


write = write_beats


class JointHMM:
    """madmom's bar-pointer state space for several meters, as one sparse forward model."""

    def __init__(self, meters=(3, 4), min_bpm=55.0, max_bpm=215.0, transition_lambda=100,
                 observation_lambda=16, fps=FPS):
        self.fps = fps
        from madmom.features.beats_hmm import (BarStateSpace, BarTransitionModel,
                                               MultiPatternStateSpace,
                                               MultiPatternTransitionModel,
                                               RNNDownBeatTrackingObservationModel)
        spaces = [BarStateSpace(m, 60.0 * fps / max_bpm, 60.0 * fps / min_bpm) for m in meters]
        tms = [BarTransitionModel(s, transition_lambda) for s in spaces]
        self.space = MultiPatternStateSpace(spaces)
        self.tm = MultiPatternTransitionModel(tms)
        self.om = RNNDownBeatTrackingObservationModel(self.space, observation_lambda)
        S = self.tm.num_states
        # Row j holds the predecessors of state j, so forward is M @ fwd.
        self.M = sp.csr_matrix((self.tm.probabilities, self.tm.states, self.tm.pointers),
                               shape=(S, S))
        self.MT = self.M.T.tocsr()
        self.S = S
        self.meters = np.asarray(meters)
        self.positions = self.space.state_positions
        self.intervals = self.space.state_intervals
        self.patterns = self.space.state_patterns
        self.ptr = self.om.pointers

    def densities(self, act):
        with np.errstate(divide="ignore", invalid="ignore"):
            ld = self.om.log_densities(act)
        ld = np.nan_to_num(ld, nan=-30.0, neginf=-30.0)
        return np.exp(ld)

    def tempo_window(self, lo_bpm, hi_bpm, penalty=0.97):
        """Per-frame weight on every state: 1 inside the operator's window, `penalty`
        outside, so an octave outside the window has to keep out-arguing it every frame."""
        bpm = 60.0 * self.fps / self.intervals
        return np.where((bpm >= lo_bpm) & (bpm <= hi_bpm), 1.0, penalty)

    def run(self, act, lag=0, window=None):
        dens = self.densities(act)
        T = len(act)
        state_weight = None if window is None else self.tempo_window(*window)
        fwd = np.full(self.S, 1.0 / self.S)
        hist = np.zeros((lag + 1, self.S)) if lag else None
        dhist = []
        best = np.zeros(T, dtype=np.int64)
        pattern_mass = np.zeros((T, len(self.meters)))
        for t in range(T):
            f = self.M @ fwd
            f *= dens[t, self.ptr]
            if state_weight is not None:
                f *= state_weight
            c = f.sum()
            if not c > 0:
                f = np.full(self.S, 1.0 / self.S)
            else:
                f /= c
            fwd = f
            for k in range(len(self.meters)):
                pattern_mass[t, k] = f[self.patterns == k].sum()
            if lag == 0:
                best[t] = int(np.argmax(f))
            else:
                hist = np.roll(hist, -1, axis=0)
                hist[-1] = f
                dhist.append(dens[t, self.ptr])
                if len(dhist) > lag + 1:
                    dhist.pop(0)
                if t >= lag:
                    # posterior at t-lag given everything up to t: fwd[t-lag] * beta
                    beta = np.ones(self.S)
                    for s in range(len(dhist) - 1, 0, -1):
                        beta = self.MT @ (beta * dhist[s])
                        beta /= beta.max()
                    post = hist[0] * beta
                    best[t - lag] = int(np.argmax(post))
        if lag:
            best[T - lag:] = best[T - lag - 1]
        return best, pattern_mass

    def decode(self, best, min_fraction=0.5):
        """Beats where the MAP state crosses into a new beat of the bar; downbeat at beat 0."""
        pos = self.positions[best]
        beat_idx = np.floor(pos).astype(int)
        meter = self.meters[self.patterns[best]]
        interval = self.intervals[best]
        times, labels = [], []
        last = -1e9
        for t in range(1, len(best)):
            crossed = beat_idx[t] != beat_idx[t - 1] or pos[t] < pos[t - 1]
            if crossed and (t - last) >= min_fraction * interval[t]:
                times.append(t / self.fps)
                labels.append(beat_idx[t] + 1)
                last = t
        return np.array(times), np.array(labels), 60.0 * self.fps / interval, meter


def upsample(act, factor=2):
    """Linear interpolation of the activations to factor x the frame rate."""
    T = len(act)
    x = np.arange(T)
    xi = np.arange(0, T - 1 + 1e-9, 1.0 / factor)
    return np.stack([np.interp(xi, x, act[:, k]) for k in range(act.shape[1])], axis=1)


def one(job):
    stem, tag, variants = job
    from madmom.features.beats import DBNBeatTrackingProcessor
    from madmom.features.downbeats import DBNDownBeatTrackingProcessor
    t0 = time.time()
    act = load_activations(stem, tag)
    any_beat = act.sum(1)
    notes = []
    prefix = f"{stem}.{tag}" if tag != "nofold" else stem

    if "viterbi_down" in variants:
        p = OUT / f"{prefix}.viterbi_down.beats"
        if not p.exists():
            res = DBNDownBeatTrackingProcessor(beats_per_bar=[3, 4], fps=FPS)(act)
            write(p, res[:, 0], res[:, 1])
    if "viterbi_beat" in variants:
        p = OUT / f"{prefix}.viterbi_beat.beats"
        if not p.exists():
            b = DBNBeatTrackingProcessor(fps=FPS)(any_beat)
            write(p, b, np.zeros(len(b)))
    if "online_beat" in variants:
        p = OUT / f"{prefix}.online_beat.beats"
        if not p.exists():
            proc = DBNBeatTrackingProcessor(fps=FPS, online=True)
            b = proc.process_online(any_beat, reset=True)
            write(p, b, np.zeros(len(b)))
    if "viterbi_down100" in variants:
        p = OUT / f"{prefix}.viterbi_down100.beats"
        if not p.exists():
            res = DBNDownBeatTrackingProcessor(beats_per_bar=[3, 4], fps=2 * FPS)(upsample(act))
            write(p, res[:, 0], res[:, 1])

    for variant in ("fwd", "fwd_lag25", "fwd100", "fwd_win", "fwd100_win"):
        if variant not in variants:
            continue
        p = OUT / f"{prefix}.{variant}.beats"
        if p.exists():
            continue
        if variant == "fwd100":
            hmm = JointHMM(fps=2 * FPS)
            best, pmass = hmm.run(upsample(act), lag=0)
        elif variant == "fwd100_win":
            hmm = JointHMM(fps=2 * FPS)
            best, pmass = hmm.run(upsample(act), lag=0, window=(70.0, 140.0))
        elif variant == "fwd_win":
            hmm = JointHMM()
            best, pmass = hmm.run(act, lag=0, window=(70.0, 140.0))
        else:
            hmm = JointHMM()
            best, pmass = hmm.run(act, lag=25 if variant == "fwd_lag25" else 0)
        times, labels, bpm, meter = hmm.decode(best)
        write(p, times, labels)
        np.save(OUT / f"{prefix}.{variant}.tempo.npy", bpm.astype(np.float32))
        np.save(OUT / f"{prefix}.{variant}.meter.npy", meter.astype(np.int8))
        notes.append(f"{variant}: {len(times)} beats, median {np.median(bpm[250:]):.1f} BPM, "
                     f"meter4 {np.mean(meter[250:] == 4):.2f}")
    return f"{time.time() - t0:6.1f}s  {stem:<45} {tag:<6} " + "; ".join(notes)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--tags", default="nofold")
    ap.add_argument("--variants", default="viterbi_down,viterbi_beat,online_beat,fwd,fwd_lag25")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    stems = sorted(p.name.replace(".nofold.trace", "") for p in TRACES.glob("*.nofold.trace"))
    if a.names:
        stems = [s for s in stems if any(n in s for n in a.names)]
    variants = set(a.variants.split(","))
    jobs = [(s, t, variants) for t in a.tags.split(",") for s in stems]
    with ProcessPoolExecutor(max_workers=6) as pool:
        for line in pool.map(one, jobs):
            print(line, flush=True)
