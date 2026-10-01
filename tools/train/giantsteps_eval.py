"""Tempo accuracy of takt4's tracker on GiantSteps, per weight set — the third gate.

    python tools/train/giantsteps_eval.py generic electronic
    python tools/train/giantsteps_eval.py generic C:/path/to/some.bin --jobs 8 --limit 50

GiantSteps is 664 two-minute Beatport previews of electronic
dance music with tempo annotations only — no beats, so it cannot train the model or
score beat F, but it is the one public EDM set whose tempi were annotated by people
twice (2015, and the 2018 crowd re-annotation in `annotations_v2/`, which is the one
used here, falling back to 2015 where v2 has none). Every preview goes through
`takt4-cli track --bpm off` exactly as tools/evaluate.py runs Ballroom; the published
tempo is the median of the beat file's BPM column after five seconds, and accuracy 1 is
within 4 %, accuracy 2 also admits the octave and the triple — tools/refeval/score.py's
definitions.

Writes WORK/giantsteps.<weights>.json per weight set and prints the comparison. A preview
the CLI fails to run on is reported and left out of the mean, and the script then exits 1:
the numbers are not a measurement of the set.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import DATASETS, WORK, save_json  # noqa: E402


def cli_path():
    """tools/refeval/common.py's `cli_path`, loaded under its own name: both directories
    have a `common` module."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "refeval_common", Path(__file__).resolve().parents[1] / "refeval" / "common.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.cli_path()

GS = DATASETS / "giantsteps-tempo"


def annotations():
    """{stem: bpm} from annotations_v2/tempo, then annotations/tempo for the rest."""
    out = {}
    for d in (GS / "annotations_v2" / "tempo", GS / "annotations" / "tempo"):
        for p in sorted(d.glob("*.bpm")):
            if p.stem not in out:
                try:
                    bpm = float(p.read_text(encoding="utf-8").strip().split()[0])
                except (ValueError, IndexError):
                    continue
                if bpm > 0:
                    out[p.stem] = bpm
    return out


def run_one(cli, wav, weights, scratch):
    """(stem, published tempo, error). A run that found no tempo publishes 0 and is a miss;
    a CLI that did not run has an error and no score at all."""
    out = Path(scratch) / (wav.stem + ".beats")
    cmd = [str(cli), "track", str(wav), "--out", str(out), "--bpm", "off", "--weights", weights]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not out.exists():
        return wav.stem, None, f"exit {r.returncode}: " + ((r.stderr or r.stdout).strip()[-200:]
                                                          or "no beat file written")
    rows = np.loadtxt(out, ndmin=2)
    if rows.size == 0 or rows.shape[1] < 3:
        return wav.stem, 0.0, None
    keep = rows[:, 0] >= 5.0
    return wav.stem, float(np.median(rows[keep, 2])) if keep.any() else 0.0, None


def accuracy(sys_bpm, ref_bpm, tol=0.04):
    if not sys_bpm or not ref_bpm:
        return 0, 0
    a1 = int(abs(sys_bpm - ref_bpm) / ref_bpm <= tol)
    a2 = int(any(abs(sys_bpm * k - ref_bpm) / ref_bpm <= tol for k in (1, 2, 0.5, 3, 1 / 3)))
    return a1, a2


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("weights", nargs="+", help="set names or .bin paths, as takt4-cli takes them")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--limit", type=int, default=None)
    a = ap.parse_args(argv)
    cli = cli_path()
    refs = annotations()
    wavs = sorted(p for p in (GS / "audio").glob("*.wav") if p.stem in refs)
    if a.limit:
        wavs = wavs[:a.limit]
    print(f"{cli}\n{len(wavs)} previews with a tempo annotation")
    results = {}
    any_failed = False
    for weights in a.weights:
        name = Path(weights).stem if ("/" in weights or "\\" in weights or weights.endswith(".bin")) else weights
        with tempfile.TemporaryDirectory(prefix="takt4-gs-") as scratch, \
                ThreadPoolExecutor(max_workers=a.jobs) as pool:
            rows = list(pool.map(lambda w: run_one(cli, w, weights, scratch), wavs))
        per, failures = {}, {}
        for stem, bpm, error in rows:
            if error is not None:
                # Not a wrong tempo: the CLI did not run, which says nothing about the
                # weights. Counted as a miss, a broken build read as a worse model (P8).
                failures[stem] = error
                continue
            a1, a2 = accuracy(bpm, refs[stem])
            per[stem] = {"ref": refs[stem], "published": bpm, "acc1": a1, "acc2": a2}
        summary = {"n": len(per), "failed": len(failures),
                   "acc1": float(np.mean([p["acc1"] for p in per.values()])) if per else 0.0,
                   "acc2": float(np.mean([p["acc2"] for p in per.values()])) if per else 0.0}
        results[name] = summary
        save_json(WORK / f"giantsteps.{name}.json", {"weights": weights, "cli": str(cli),
                                                       "summary": summary, "tracks": per,
                                                       "failures": failures})
        print(f"{name:<24} n={summary['n']}  tempo acc1 {summary['acc1']:.4f}  acc2 {summary['acc2']:.4f}"
              + (f"  -- {len(failures)} of {len(rows)} FAILED to run, e.g. "
                 f"{next(iter(failures.items()))}; not a measurement of this set" if failures else ""))
        any_failed |= bool(failures)
    return 1 if any_failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
