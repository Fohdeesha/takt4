"""Score weight sets on a set's validation excerpts the way the app runs — window on or
off, bars of four or of three and four — through takt4-cli.

    python tools/train/valset_eval.py --set library --weights generic build/training/weights/electronic-library-e24.bin
    python tools/train/valset_eval.py --set library --weights generic --bpm off 70-140 --meters 3,4 4
    python tools/train/valset_eval.py --set library --weights generic --config tools/train/configs/electronic-library.yaml
    python tools/train/valset_eval.py --set library --weights ... --run electronic-library-v2   # that run's own tracks

finetune.py validates with `--bpm off` (the way the published trackers are measured), but
the application runs with the operator's 70-140 window on, and TRACKING-PROPOSAL.md
§7.7 measured bars of four alone as the better decoder on electronic material. A weight
set's number for *the rig* is the one measured the rig's way, and the difference between
the two is the octave: a track the teacher annotated at 170 and the labels halve to 85 is
scored 0.67 with the window off if the model publishes 170 — every other beat matches —
and 1.0 with the window on, which folds it. So this scores every combination asked for,
on the same excerpts finetune.py validates on (the middle two minutes of every validation
track of the set, as selected by the config's flags), and reports beat F, downbeat F,
tempo accuracy 1 and 2, and how many tracks are at the octave or half the octave of the
reference. Per-track scores go to WORK/valset/<set>.<weights>.<bpm>.<meters>.json.

The CLI is tools/refeval/common.py's (TAKT4_CLI, else the local Release build).
"""
import argparse
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (ALIGNMENT, FPS, RUNS, WORK, cli_identity, cli_path, load_json,  # noqa: E402
                    load_manifest, now, read_beats, save_json)
import finetune  # noqa: E402


def score_one(args):
    import mir_eval
    cli, weights, bpm, meters, set_name, tid, wav, t0, ref_t, ref_p = args
    with tempfile.TemporaryDirectory(prefix="takt4-vs-") as scratch:
        out = Path(scratch) / "beats.txt"
        cmd = [str(cli), "track", str(wav), "--out", str(out), "--bpm", bpm, "--meters", meters,
               "--weights", str(weights), "--seed", "1", "--confidence", "0.15"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not out.exists():
            # Why, not just that. A weight set named rather than pathed is the mistake that
            # gets made here, and every track then fails for the same reason — which used to
            # come out as a KeyError on the summary line, twelve minutes later, with the
            # actual complaint discarded. See `summary`.
            return tid, {"error": (r.stderr or r.stdout or "no output file").strip()[:200]}
        rows = np.loadtxt(out, ndmin=2)
    if rows.size == 0:
        return tid, {"F": 0.0, "dF": 0.0, "a1": 0, "a2": 0, "ratio": 0.0, "published": 0.0, "ref_bpm": 0.0, "beats": 0}
    est_t, est_p = rows[:, 0], rows[:, 1].astype(int)
    ref_b = mir_eval.beat.trim_beats(ref_t)
    ref_d = mir_eval.beat.trim_beats(ref_t[ref_p == 1])
    est_b = mir_eval.beat.trim_beats(est_t)
    est_d = mir_eval.beat.trim_beats(est_t[est_p == 1])
    beat_f = mir_eval.beat.f_measure(ref_b, est_b) if len(ref_b) and len(est_b) else 0.0
    down_f = mir_eval.beat.f_measure(ref_d, est_d) if len(ref_d) and len(est_d) else 0.0
    ref_bpm = 60.0 / np.median(np.diff(ref_t)) if len(ref_t) > 2 else 0.0
    keep = est_t >= 5.0
    published = float(np.median(rows[keep, 2])) if rows.shape[1] > 2 and keep.any() else \
        (60.0 / np.median(np.diff(est_t)) if len(est_t) > 2 else 0.0)
    a1 = int(ref_bpm > 0 and published > 0 and abs(published - ref_bpm) / ref_bpm <= 0.04)
    a2 = int(ref_bpm > 0 and published > 0 and any(abs(published * k - ref_bpm) / ref_bpm <= 0.04
                                                   for k in (1, 2, 0.5, 3, 1 / 3)))
    return tid, {"F": round(float(beat_f), 4), "dF": round(float(down_f), 4), "a1": a1, "a2": a2,
                 "ratio": round(published / ref_bpm, 3) if ref_bpm else 0.0,
                 "published": round(published, 1), "ref_bpm": round(ref_bpm, 1), "beats": int(len(est_t))}


def weights_sha256(weights):
    """The SHA-256 of a weights file; a built-in set's name has none."""
    import hashlib
    path = Path(weights)
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def run_entries(run, set_name, manifest):
    """The validation tracks a run was selected on, from its own record — which stays right
    when the manifest's split has moved since, as it did whenever a track was added before
    2026-09-26 (the audit's P2)."""
    record = RUNS / run / "val_tracks_epoch_000.json"
    if not record.exists():
        raise SystemExit(f"{record} not found: is {run!r} a run under {RUNS}?")
    ids = sorted(load_json(record).get(set_name, {}))
    if not ids:
        raise SystemExit(f"{run} validated nothing of {set_name}")
    tracks = {t["id"]: t for t in manifest["sets"][set_name]["tracks"]}
    missing = [i for i in ids if i not in tracks]
    if missing:
        raise SystemExit(f"{len(missing)} of {run}'s {set_name} validation tracks are no longer in "
                         f"the manifest, e.g. {missing[0]}")
    return [finetune.Entry(set_name, tracks[i],
                           int(np.load(finetune.feature_paths(set_name, i)[0], mmap_mode="r").shape[0]))
            for i in ids]


def summary(rows):
    ok = [r for r in rows.values() if r is not None and "error" not in r]
    bad = [r for r in rows.values() if r is None or "error" in r]
    if not ok:
        # Every track failed, which means one thing went wrong rather than eighty. Carry the
        # first complaint out so the caller can print it: this used to return an empty dict
        # and the caller then died on `s['n']`, discarding the only useful information in the
        # run. A weight set named where a path was wanted is how that happens.
        first = next((r["error"] for r in bad if r and "error" in r), "no reason reported")
        return {"n": 0, "failed": len(rows), "error": first}
    ratios = np.array([r["ratio"] for r in ok])
    return {"n": len(ok), "failed": len(bad),
            "F": float(np.mean([r["F"] for r in ok])), "dF": float(np.mean([r["dF"] for r in ok])),
            "a1": float(np.mean([r["a1"] for r in ok])), "a2": float(np.mean([r["a2"] for r in ok])),
            "doubled": int(np.sum(np.abs(ratios - 2.0) <= 0.08)),
            "halved": int(np.sum(np.abs(ratios - 0.5) <= 0.02)),
            "F_median": float(np.median([r["F"] for r in ok]))}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--set", default="library")
    ap.add_argument("--weights", nargs="+", default=["generic"])
    ap.add_argument("--bpm", nargs="+", default=["off", "70-140"])
    ap.add_argument("--meters", nargs="+", default=["3,4", "4"])
    ap.add_argument("--config", default=str(Path(__file__).resolve().parent / "configs" / "electronic-library.yaml"))
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--run", default=None,
                    help="score the tracks that run validated on (its val_tracks_epoch_000.json) "
                         "rather than the ones today's manifest and --config select")
    a = ap.parse_args(argv)

    cfg = finetune.load_config(a.config, [])
    cfg["sets"] = {a.set: 1}
    cfg["select_sets"] = [a.set]
    manifest = load_manifest()
    alignment = (load_json(ALIGNMENT) or {}).get("tracks", {})
    if a.run:
        entries = run_entries(a.run, a.set, manifest)
    else:
        _, val_sets, _ = finetune.select_tracks(cfg, manifest, alignment)
        entries = val_sets[a.set]
    entries = entries[:a.limit] if a.limit else entries
    wavs = finetune.prepare_val_wavs({a.set: entries}, cfg)
    cli = cli_path()
    identity = cli_identity(cli)
    print(f"{cli}\n{len(entries)} validation excerpts of {a.set}, {cfg['val_seconds']} s each\n", flush=True)

    results = {}
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for weights in a.weights:
            wname = Path(weights).stem if ("/" in weights or "\\" in weights or weights.endswith(".bin")) else weights
            for bpm in a.bpm:
                for meters in a.meters:
                    jobs = []
                    for e in entries:
                        wav, t0 = wavs[(a.set, e.id)]
                        times, positions = read_beats(e.beats)
                        f0, L = finetune.val_window(e, cfg["val_seconds"])
                        keep = (times >= t0) & (times < t0 + L / FPS)
                        jobs.append((cli, weights, bpm, meters, a.set, e.id, wav, t0, times[keep] - t0, positions[keep]))
                    rows = dict(pool.map(score_one, jobs))
                    s = summary(rows)
                    tag = f"{wname}.{bpm}.{meters.replace(',', '+')}"
                    results[tag] = s
                    # What was run, whole: the build (a path names whatever is there now),
                    # the weights file's hash, where the tracks came from, and which they
                    # were. The record used to be the CLI's path and the weights as typed (the
                    # 2026-09-25 audit's P16).
                    save_json(WORK / "valset" / f"{a.set}.{tag}.json",
                              {"weights": weights, "weights_sha256": weights_sha256(weights),
                               "bpm": bpm, "meters": meters, "cli": identity,
                               "tracks_from": f"run {a.run}" if a.run else f"config {a.config}",
                               "manifest_created": manifest.get("created"), "when": now(),
                               "track_ids": [e.id for e in entries], "summary": s, "tracks": rows})
                    if s["n"] == 0:
                        # Said here and the run carried on, rather than a traceback out of the
                        # format string below. One weight set that cannot be loaded must not
                        # cost the others their scores.
                        print(f"{wname:<28} bpm {bpm:<7} meters {meters:<4} ALL {s['failed']} FAILED"
                              f"  — {s['error']}", flush=True)
                        continue
                    print(f"{wname:<28} bpm {bpm:<7} meters {meters:<4} n={s['n']:3d}  beat F {s['F']:.4f} (median {s['F_median']:.3f})"
                          f"  dF {s['dF']:.4f}  acc1 {s['a1']:.3f}  acc2 {s['a2']:.3f}  doubled {s['doubled']:3d}  halved {s['halved']:3d}"
                          + (f"  ({s['failed']} failed)" if s["failed"] else ""), flush=True)
    return results


if __name__ == "__main__":
    main(sys.argv[1:])
