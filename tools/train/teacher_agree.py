"""A second, independent teacher over a set, and its agreement with the labels on disk.

Every label the library set was trained on came from Beat This!, and `agreement.tsv` is the
spread of three of its *seeds*. Seeds of one model share that model's bias, so their
agreement measures variance and not correctness: on 2026-09-09 the ten sampled tracks where
madmom sat a whole octave from the teacher had a mean seed agreement of 0.806 — the three
seeds confidently wrong together — and the seed filter caught three of them.

madmom's RNN + DBN is a different lineage. Where it agrees with the teacher the label is
worth trusting; where it splits, the label is a guess. It is *weaker* than Beat This! on
electronic music (the Raveform paper's own numbers), so a disagreement does not mean the
teacher is wrong — use this to **exclude or downweight, never to relabel**.

    python tools/train/teacher_agree.py --set library --workers 4
    python tools/train/teacher_agree.py --set library --limit 150 --out sample.tsv

Writes `<dataset>/teacher_cross.tsv`: stem, madmom BPM, teacher BPM, beat F, downbeat F,
BPM ratio. Resumable — rows already present are kept and their tracks skipped, so a run
that is killed mid-way (and it should be killed if the operator needs the machine) picks up
where it stopped. `--workers` costs that many cores for the duration; madmom is 25–150 s a
track, so the full 1,326-track library is a few hours at 4.
"""
import argparse
import os
import random
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import DATASETS  # noqa: E402

BEATS_PER_BAR = [4]     # the decoder the rig runs since 0.9.1; §7.13.3
TRIM = 5.0              # mir_eval's convention here and in tools/refeval/score.py
HEADER = "stem\tmadmom_bpm\tteacher_bpm\tbeatF\tdownbeatF\tbpm_ratio\n"


def row_text(r):
    return f"{r[0]}\t{r[1]:.2f}\t{r[2]:.2f}\t{r[3]:.4f}\t{r[4]:.4f}\t{r[5]:.3f}\n"


def load_beats(path):
    rows = np.loadtxt(path, ndmin=2)
    if rows.size == 0:
        return np.zeros(0), np.zeros(0, dtype=int)
    labels = rows[:, 1].astype(int) if rows.shape[1] > 1 else np.zeros(len(rows), dtype=int)
    return rows[:, 0], labels


def bpm_of(times):
    if len(times) < 3:
        return 0.0
    gaps = np.diff(times)
    gaps = gaps[gaps > 0]
    return float(60.0 / np.median(gaps)) if len(gaps) else 0.0


def _one(job):
    """madmom over one WAV. Imported inside the worker: it is slow and CUDA-free."""
    stem, wav, ann = job
    try:
        import warnings
        warnings.filterwarnings("ignore")
        from madmom.features.downbeats import (DBNDownBeatTrackingProcessor,
                                               RNNDownBeatProcessor)
        t0 = time.time()
        act = RNNDownBeatProcessor()(str(wav))
        got = DBNDownBeatTrackingProcessor(beats_per_bar=BEATS_PER_BAR, fps=100)(act)
        return stem, got[:, 0], got[:, 1].astype(int), str(ann), time.time() - t0, None
    except Exception as exc:                                          # noqa: BLE001
        return stem, None, None, str(ann), 0.0, repr(exc)[:300]


def score(mt, ml, ann_path):
    import mir_eval
    rt, rl = load_beats(ann_path)
    if len(mt) < 3 or len(rt) < 3:
        return None
    end = max(rt[-1], mt[-1]) - TRIM
    r = rt[(rt >= TRIM) & (rt <= end)]
    m = mt[(mt >= TRIM) & (mt <= end)]
    rd = rt[(rl == 1) & (rt >= TRIM) & (rt <= end)]
    md = mt[(ml == 1) & (mt >= TRIM) & (mt <= end)]
    if not len(r) or not len(m):
        return None
    f = mir_eval.beat.f_measure(r, m)
    df = mir_eval.beat.f_measure(rd, md) if len(rd) and len(md) else 0.0
    rb, mb = bpm_of(rt), bpm_of(mt)
    return mb, rb, f, df, (mb / rb if rb else 0.0)


def summarise(rows):
    f = np.array([r[3] for r in rows])
    ratio = np.array([r[5] for r in rows])
    print(f"\nscored {len(rows)} tracks")
    print(f"beat F      mean {f.mean():.3f}   median {np.median(f):.3f}")
    print(f"downbeat F  mean {np.mean([r[4] for r in rows]):.3f}")
    for thr in (0.5, 0.7, 0.8, 0.9):
        print(f"  below {thr}: {(f < thr).sum():4d}  ({(f < thr).mean() * 100:.1f}%)")
    octave = (np.abs(ratio - 2.0) < 0.1) | (np.abs(ratio - 0.5) < 0.05)
    print(f"an octave from the teacher: {octave.sum()} "
          f"({octave.mean() * 100:.1f}%) — the labels that matter most")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", default="library", help="dataset under references/datasets/")
    ap.add_argument("--out", type=Path, default=None,
                    help="default <dataset>/teacher_cross.tsv")
    ap.add_argument("--limit", type=int, default=0, help="a seeded sample, 0 = every track")
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--workers", type=int, default=4)
    a = ap.parse_args()

    root = DATASETS / a.set
    audio, annos = root / "audio", root / "annotations"
    if not audio.is_dir() or not annos.is_dir():
        raise SystemExit(f"{root}: expected audio/ and annotations/")
    out = a.out or (root / "teacher_cross.tsv")

    done = {}
    if out.exists():
        for line in out.read_text(encoding="utf-8").splitlines()[1:]:
            p = line.split("\t")
            if len(p) >= 6:
                try:                                  # a kill can truncate the last line
                    done[p[0]] = tuple([p[0]] + [float(x) for x in p[1:6]])
                except ValueError:
                    pass
        print(f"resuming: {len(done)} rows already in {out.name}")

    stems = sorted(p.stem for p in audio.glob("*.wav"))
    if a.limit:
        random.seed(a.seed)
        random.shuffle(stems)
        stems = stems[:a.limit]
    jobs = [(s, audio / f"{s}.wav", annos / f"{s}.beats") for s in stems
            if s not in done and (annos / f"{s}.beats").exists()]
    print(f"{len(stems)} tracks in {a.set}, {len(jobs)} to do, {a.workers} workers",
          flush=True)

    rows, errors, n = list(done.values()), [], 0
    existed = out.exists()

    # Append and flush each row as it is scored, rather than holding the lot until the
    # end. A run launched detached has no console to Ctrl-C, so the way it stops when the
    # operator wants the machine back is Stop-Process — and a buffered run loses its hours.
    log = open(out, "a", encoding="utf-8", newline="\n")
    if not existed:
        log.write(HEADER)
        log.flush()
    try:
        with ProcessPoolExecutor(max_workers=a.workers) as ex:
            for stem, mt, ml, ann, dt, err in ex.map(_one, jobs):
                n += 1
                if err:
                    errors.append((stem, err))
                    continue
                got = score(mt, ml, ann)
                if got:
                    rows.append((stem,) + got)
                    log.write(row_text(rows[-1]))
                    log.flush()
                if n % 25 == 0:
                    print(f"  {n}/{len(jobs)}  last {dt:.0f}s  "
                          f"({len(errors)} errors)", flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted — writing what is done so far", flush=True)
    finally:
        log.close()

    # Rewrite sorted, which is also what folds a resumed run's two halves together.
    rows.sort(key=lambda r: r[0])
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(HEADER)
        for r in rows:
            fh.write(row_text(r))
    print(f"wrote {out} ({len(rows)} rows)")
    if errors:
        print(f"{len(errors)} errors, first: {errors[0][0]} {errors[0][1]}")
    if rows:
        summarise(rows)


if __name__ == "__main__":
    main()
