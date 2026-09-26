"""Extract the features BeatNet+ trains on, for every track in the manifest.

    python tools/train/features.py                 # everything not yet extracted
    python tools/train/features.py --sets ballroom --jobs 4
    python tools/train/features.py --force         # redo existing files

Per track, under WORK/features/<set>/:

    <id>.npy      (frames, 288) float16 — BeatNet+'s LOG_SPECT features, computed by
                  tools/beatnet_features.py's restatement of that chain (the one the C++
                  front end is tested against), from the audio loaded exactly as
                  prepare_data.py loads it: librosa.load(sr=22050, mono=True).
    <id>.gt.npy   (frames,) int8 — the class per frame, 0 beat, 1 downbeat, 2 neither,
                  from prepare_data.py's own build_ground_truth (imported, not restated),
                  so a beat lands on the same frame it would in BeatNet+'s pipeline.

float16 halves 52 GB to 26 GB on the system SSD; the quantisation error is printed as it
goes (about 1e-3 on values up to ~3, three orders below anything the network cares
about) and the loader hands the network float32. Runs `--jobs` processes below normal
priority; the desktop stays responsive.
"""
import argparse
import multiprocessing as mp
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (FEATURES, SAMPLE_RATE, add_tools_path, feature_paths,  # noqa: E402
                    labels_from_beats, load_manifest, lower_priority, save_json)

PIPE = None


def _init():
    global PIPE
    add_tools_path()
    from beatnet_features import FeaturePipeline
    PIPE = FeaturePipeline()


def _one(job):
    set_name, track_id, wav, beats, gt_only = job
    import librosa
    feat_path, gt_path = feature_paths(set_name, track_id)
    try:
        if gt_only:
            # The audio has not changed, the labels have: only the class per frame is redone.
            frames = int(np.load(feat_path, mmap_mode="r").shape[0])
            err = 0.0
        else:
            audio, _ = librosa.load(wav, sr=SAMPLE_RATE, mono=True)
            feats = PIPE.features(audio)                   # (frames, 288) float32
            frames = int(feats.shape[0])
            half = feats.astype(np.float16)
            err = float(np.abs(half.astype(np.float32) - feats).max())
            feat_path.parent.mkdir(parents=True, exist_ok=True)
            np.save(feat_path, half)
        classes = labels_from_beats(beats, frames)
        np.save(gt_path, classes)
        return set_name, track_id, frames, int((classes == 0).sum()), \
            int((classes == 1).sum()), err, None
    except Exception as e:  # noqa: BLE001 - report, keep going
        return set_name, track_id, 0, 0, 0, 0.0, repr(e)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sets", nargs="*", default=None)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--gt-only", action="store_true",
                    help="rebuild only the class-per-frame files from the manifest's beats, for "
                         "every track whose features exist (after relabelling)")
    ap.add_argument("--limit", type=int, default=None)
    a = ap.parse_args(argv)
    lower_priority()

    manifest = load_manifest()
    jobs = []
    for set_name, info in manifest["sets"].items():
        if a.sets and set_name not in a.sets:
            continue
        for t in info["tracks"]:
            feat_path, gt_path = feature_paths(set_name, t["id"])
            if a.gt_only:
                if feat_path.exists():
                    jobs.append((set_name, t["id"], t["wav"], t["beats"], True))
                continue
            if not a.force and feat_path.exists() and gt_path.exists():
                continue
            jobs.append((set_name, t["id"], t["wav"], t["beats"], False))
    if a.limit:
        jobs = jobs[:a.limit]
    print(f"{len(jobs)} tracks to extract with {a.jobs} processes -> {FEATURES}", flush=True)
    if not jobs:
        return

    t0 = time.time()
    done, failed, worst_err, frames_total = 0, [], 0.0, 0
    with mp.Pool(a.jobs, initializer=_init) as pool:
        for set_name, tid, frames, nb, nd, err, error in pool.imap_unordered(_one, jobs, chunksize=2):
            done += 1
            if error:
                failed.append((set_name, tid, error))
                print(f"  FAILED {set_name}/{tid}: {error}", flush=True)
                continue
            frames_total += frames
            worst_err = max(worst_err, err)
            if done % 100 == 0 or done == len(jobs):
                rate = done / (time.time() - t0)
                print(f"  {done}/{len(jobs)}  {rate:.1f} tracks/s, eta {(len(jobs) - done) / rate / 60:.1f} min; "
                      f"last {set_name}/{tid}: {frames} frames, {nb} beats, {nd} downbeats, "
                      f"float16 err {err:.1e}", flush=True)
    print(f"done in {(time.time() - t0) / 60:.1f} min: {done - len(failed)} extracted, "
          f"{frames_total / 50 / 3600:.1f} h of frames, worst float16 error {worst_err:.2e}, "
          f"{len(failed)} failed")
    if failed:
        save_json(FEATURES / "failed.json", failed)


if __name__ == "__main__":
    main(sys.argv[1:])
