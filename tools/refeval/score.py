"""Score any number of beat-file directories against a reference directory with mir_eval.

    python tools/refeval/score.py --ref ref_beatthis takt4:nofold takt4:fold70 dec:fwd100
    python tools/refeval/score.py --min-agree 0.8 --quiet takt4:nofold     # the agreed-15

Each system argument is DIR or DIR:TAG, where files are <stem>[.TAG].beats with
'<seconds>\\t<beat in bar>[\\t<bpm>]' rows and downbeat = 1. mir_eval's conventions: 70 ms
tolerance, the first and last five seconds trimmed — exactly as tools/evaluate.py scores
Ballroom, so a number here and a number there mean the same thing.

The published tempo is the median of the beat file's third column over the beats after
5 s when there is one (takt4's `--out` writes it), and 60 over the median beat gap when
there is not. Tempo accuracy 1 is within 4 %; accuracy 2 also admits the octave and the
triple.

`--json FILE` writes everything the table shows, per track and summarised, which is what
gate.py compares against its committed baseline.
"""
import argparse
import json
import sys

import mir_eval
import numpy as np

from common import HARD_EIGHT, REFS, beats_path, load_beats, stems, system_dir

METRICS = ("F", "CMLt", "AMLt", "dF", "a1", "a2")


def tempo_of(times):
    if len(times) < 3:
        return 0.0
    gaps = np.diff(times)
    gaps = gaps[gaps > 0]
    return 60.0 / np.median(gaps)


def tempo_acc(sys_bpm, ref_bpm, tol=0.04):
    if ref_bpm <= 0 or sys_bpm <= 0:
        return 0, 0
    acc1 = int(abs(sys_bpm - ref_bpm) / ref_bpm <= tol)
    acc2 = int(any(abs(sys_bpm * k - ref_bpm) / ref_bpm <= tol for k in (1, 2, 0.5, 3, 1 / 3)))
    return acc1, acc2


def score_pair(ref_t, ref_l, sys_t, sys_l, start=5.0):
    end = max(ref_t[-1], sys_t[-1] if len(sys_t) else 0) - 5.0
    r = ref_t[(ref_t >= start) & (ref_t <= end)]
    s = sys_t[(sys_t >= start) & (sys_t <= end)]
    rd = ref_t[(ref_l == 1) & (ref_t >= start) & (ref_t <= end)]
    sd = sys_t[(sys_l == 1) & (sys_t >= start) & (sys_t <= end)]
    out = {}
    out["F"] = mir_eval.beat.f_measure(r, s) if len(r) and len(s) else 0.0
    cmlc, cmlt, amlc, amlt = mir_eval.beat.continuity(r, s) if len(r) and len(s) else (0, 0, 0, 0)
    out["CMLt"] = cmlt
    out["AMLt"] = amlt
    out["dF"] = mir_eval.beat.f_measure(rd, sd) if len(rd) and len(sd) else 0.0
    return out


def published_tempo(sys_t, sys_bpm_col):
    if sys_bpm_col is not None:
        keep = sys_t >= 5.0
        return float(np.median(sys_bpm_col[keep])) if keep.any() else 0.0
    return tempo_of(sys_t)


def score_systems(system_specs, ref="ref_beatthis", ref2="ref_madmom", tracks=None):
    """Every system over every track of `ref`.

    Returns {"tracks": {stem: {"ref_bpm", "agree", systems: {spec: {bpm, F, CMLt, AMLt, dF,
    a1, a2}}}}, "systems": [spec...]}. A track the system has no file for is absent from
    that system's entry; a track with fewer than three beats scores as zeros.
    """
    ref_dir = REFS / ref
    ref2_dir = REFS / ref2
    systems = [system_dir(spec) for spec in system_specs]
    out = {"ref": ref, "ref2": ref2, "systems": [s[0] for s in systems], "tracks": {}}
    for stem in stems(ref):
        if tracks and not any(t in stem for t in tracks):
            continue
        ref_t, ref_l, _ = load_beats(ref_dir / f"{stem}.beats")
        if len(ref_t) < 3:
            continue
        row = {"ref_bpm": tempo_of(ref_t), "agree": None, "systems": {}}
        p2 = ref2_dir / f"{stem}.beats"
        if p2.exists():
            r2_t, r2_l, _ = load_beats(p2)
            if len(r2_t) > 3:
                row["agree"] = score_pair(ref_t, ref_l, r2_t, r2_l)["F"]
        for spec, d, tag in systems:
            p = beats_path(d, stem, tag)
            if not p.exists():
                continue
            sys_t, sys_l, sys_bpm_col = load_beats(p)
            if len(sys_t) < 3:
                row["systems"][spec] = {"bpm": 0.0, "F": 0.0, "CMLt": 0.0, "AMLt": 0.0,
                                        "dF": 0.0, "a1": 0, "a2": 0, "beats": int(len(sys_t))}
                continue
            m = score_pair(ref_t, ref_l, sys_t, sys_l)
            bpm = published_tempo(sys_t, sys_bpm_col)
            a1, a2 = tempo_acc(bpm, row["ref_bpm"])
            m.update({"bpm": bpm, "a1": a1, "a2": a2, "beats": int(len(sys_t))})
            row["systems"][spec] = m
        out["tracks"][stem] = row
    return out


def summarise(scored, min_agree=0.0, exclude=()):
    """Mean of every metric per system over the tracks passing the agreement floor and not
    in `exclude`. Returns {spec: {"n": count, metric: mean}}."""
    summary = {}
    for spec in scored["systems"]:
        values = {k: [] for k in METRICS}
        for stem, row in scored["tracks"].items():
            if stem in exclude:
                continue
            if row["agree"] is not None and row["agree"] < min_agree:
                continue
            m = row["systems"].get(spec)
            if m is None:
                continue
            for k in METRICS:
                values[k].append(m[k])
        if values["F"]:
            summary[spec] = {"n": len(values["F"]),
                             **{k: float(np.mean(v)) for k, v in values.items()}}
    return summary


def print_table(scored, quiet=False):
    systems = scored["systems"]
    print(f"{'track':<40} {'refBPM':>7} {'agree':>6} | " +
          " | ".join(f"{s[:22]:^34}" for s in systems))
    print(f"{'':<40} {'':>7} {'':>6} | " +
          " | ".join(f"{'bpm':>6} {'a1':>2} {'F':>5} {'CMLt':>5} {'AMLt':>5} {'dF':>5}"
                     for _ in systems))
    if not quiet:
        for stem, row in scored["tracks"].items():
            agree = f"{row['agree']:6.3f}" if row["agree"] is not None else ""
            cells = []
            for spec in systems:
                m = row["systems"].get(spec)
                if m is None:
                    cells.append(f"{'-':>34}")
                elif m["beats"] < 3:
                    cells.append(f"{'(none)':>34}")
                else:
                    cells.append(f"{m['bpm']:6.1f} {m['a1']:>2} {m['F']:5.3f} {m['CMLt']:5.3f} "
                                 f"{m['AMLt']:5.3f} {m['dF']:5.3f}")
            print(f"{stem[:40]:<40} {row['ref_bpm']:7.1f} {agree:>6} | " + " | ".join(cells))
    print()


def print_summary(scored, min_agree, exclude=(), label=None):
    summary = summarise(scored, min_agree, exclude)
    if label:
        print(label)
    for spec, t in summary.items():
        print(f"{spec:<24} n={t['n']:2d}  beat F {t['F']:.3f}  CMLt {t['CMLt']:.3f}  "
              f"AMLt {t['AMLt']:.3f}  downbeat F {t['dF']:.3f}  "
              f"tempo acc1 {t['a1']:.3f}  acc2 {t['a2']:.3f}")
    return summary


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", default="ref_beatthis")
    ap.add_argument("--ref2", default="ref_madmom", help="second reference, for agreement")
    ap.add_argument("systems", nargs="+")
    ap.add_argument("--tracks", nargs="*", default=None)
    ap.add_argument("--min-agree", type=float, default=0.0,
                    help="only count tracks where the two references agree at least this well")
    ap.add_argument("--quiet", action="store_true", help="summary only")
    ap.add_argument("--json", default=None, help="write per-track scores and summaries here")
    a = ap.parse_args(argv)

    scored = score_systems(a.systems, a.ref, a.ref2, a.tracks)
    print_table(scored, a.quiet)
    agree = [r["agree"] for r in scored["tracks"].values() if r["agree"] is not None]
    if agree:
        print(f"reference agreement ({a.ref} vs {a.ref2}) beat F: mean {np.mean(agree):.3f}, "
              f"min {np.min(agree):.3f}, tracks below 0.8: {sum(x < 0.8 for x in agree)}")
    print_summary(scored, a.min_agree)
    if a.json:
        scored["summary_all"] = summarise(scored)
        scored["summary_agreed"] = summarise(scored, 0.8)
        scored["summary_hard"] = summarise(scored, 0.0, exclude=tuple(
            s for s in scored["tracks"] if s not in HARD_EIGHT))
        with open(a.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(scored, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"wrote {a.json}")


if __name__ == "__main__":
    main(sys.argv[1:])
