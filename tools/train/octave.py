"""Decide the octave of every fast annotation from the kick.

    python tools/train/octave.py                # every electronic-set track at >= 150 BPM
    python tools/train/octave.py --dry-run      # report only
    python tools/train/octave.py --min-bpm 140

Whether drum-and-bass-style material, annotated at the fast octave (Raveform's 160–180
BPM, osu's 180–200), should be relabelled at half tempo before training or kept at the
fast octave as the genre's convention was an open question. Measured on 2026-09-08, a
model fine-tuned on those labels doubles the tempo of half-time
material under the shipped causal decoder — Jensen Interceptor from 85 to 170, Defang
from 106 to 188 — because it has learned that a kick on alternate beats is still the
fast grid. The operator's convention is the opposite (Clutch Pearlers ~92), and it
is the kick's: a track whose kick falls on every beat is at the tempo its annotation
says; one whose kick falls on every other beat is at half of it.

So, per track annotated at `--min-bpm` or faster, the kick-band onset function
(check.py's: the positive spectral difference of the nine filterbank bands below 140 Hz,
best of ±2 frames, first two seconds skipped) is averaged over the even-indexed beats and
over the odd-indexed ones. If one parity carries at least `--ratio` times the other, the
track is relabelled at half tempo: the stronger parity's beats are kept as beats, and
the bar becomes four of them — two of the old bars — with the downbeat on the kept beat
at or after every second old downbeat, starting from the first. Otherwise the annotation
stands. The new file is `<id>.half.beats` beside the old one, the manifest's `beats`
points at it (`beats_original` keeps the old path, `octave` says "half"), and the
track's `.gt.npy` is rebuilt from it; the features are untouched.
"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (FPS, MANIFEST, NUM_BANDS, feature_paths, labels_from_beats,  # noqa: E402
                    load_manifest, read_beats, save_json, tempo_of, write_beats)
from check import KICK_BANDS, REACH, SKIP, local_max  # noqa: E402

ELECTRONIC = ("raveform", "osu2beat", "harmonix")


def parity_ratio(feats, times):
    """(mean kick on even beats, mean kick on odd beats) after SKIP seconds."""
    kick = feats[:, [NUM_BANDS + j for j in KICK_BANDS]].astype(np.float32).sum(axis=1)
    lmax = local_max(kick)
    frames = np.floor(times * FPS).astype(int)
    keep = (frames >= SKIP * FPS) & (frames < len(lmax))
    idx = np.arange(len(times))[keep]
    frames = frames[keep]
    even = lmax[frames[idx % 2 == 0]]
    odd = lmax[frames[idx % 2 == 1]]
    if len(even) < 8 or len(odd) < 8:
        return None, None
    return float(even.mean()), float(odd.mean())


def halve(times, positions, keep_parity):
    """Beats of one parity, re-barred four to the bar from the first old downbeat."""
    kept = np.arange(len(times)) % 2 == keep_parity
    t = times[kept]
    old_pos = positions[kept]
    old_index = np.arange(len(times))[kept]
    downs = np.where(positions == 1)[0]
    # Every second old downbeat, from the first: the kept beat at or after it is a "1".
    chosen = downs[::2]
    ones = set()
    for d in chosen:
        after = np.where(old_index >= d)[0]
        if len(after):
            ones.add(int(after[0]))
    new_pos = np.zeros(len(t), dtype=int)
    if not ones:
        new_pos[:] = ((np.arange(len(t))) % 4) + 1
        return t, new_pos
    first = min(ones)
    new_pos[:] = ((np.arange(len(t)) - first) % 4) + 1
    return t, new_pos


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--min-bpm", type=float, default=150.0)
    ap.add_argument("--ratio", type=float, default=2.0)
    ap.add_argument("--all", action="store_true",
                    help="halve every track at --min-bpm or faster, whatever the kick says (the "
                         "operator's convention: everything above ~150 is felt at half time); the "
                         "kick still chooses which beats are kept, the stronger parity")
    ap.add_argument("--sets", nargs="*", default=list(ELECTRONIC))
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)

    manifest = load_manifest()
    for set_name in a.sets:
        info = manifest["sets"][set_name]
        counts = {"fast": 0, "halved": 0, "kept": 0, "undecided": 0, "restored": 0}
        ratios = []
        def restore(t, original, times, positions, frames):
            """Back to the annotation: the manifest and — the part that was missing until
            2026-09-09 — the class-per-frame file, which a halving had rewritten."""
            t["beats"], t["octave"] = str(original), "as annotated"
            if not a.dry_run:
                np.save(feature_paths(set_name, t["id"])[1], labels_from_beats(original, frames))

        for t in info["tracks"]:
            # Always start from the original annotation, so the script is idempotent.
            original = Path(t.get("beats_original", t["beats"]))
            times, positions = read_beats(original)
            bpm = tempo_of(times)
            feat_path, gt_path = feature_paths(set_name, t["id"])
            was_half = t.get("octave") == "half"
            if bpm < a.min_bpm or not feat_path.exists() or len(times) < 16:
                if was_half and feat_path.exists():
                    restore(t, original, times, positions, int(np.load(feat_path, mmap_mode="r").shape[0]))
                    counts["restored"] += 1
                continue
            counts["fast"] += 1
            feats = np.load(feat_path)
            even, odd = parity_ratio(feats, times)
            if even is None:
                even = odd = 1.0
                counts["undecided"] += 1
                if not a.all:
                    if was_half:
                        restore(t, original, times, positions, feats.shape[0])
                        counts["restored"] += 1
                    continue
            ratio = max(even, odd) / max(min(even, odd), 1e-6)
            ratios.append(ratio)
            t["kick_parity"] = {"even": round(even, 4), "odd": round(odd, 4), "ratio": round(ratio, 3)}
            if ratio < a.ratio and not a.all:
                counts["kept"] += 1
                if was_half:
                    restore(t, original, times, positions, feats.shape[0])
                    counts["restored"] += 1
                else:
                    t["octave"] = "as annotated"
                    t["beats"] = str(original)
                continue
            counts["halved"] += 1
            t["octave_rule"] = "all" if a.all else f"kick parity >= {a.ratio}"
            keep = 0 if even >= odd else 1
            new_t, new_pos = halve(times, positions, keep)
            half_path = original.with_name(original.stem.replace(".half", "") + ".half.beats")
            t["beats_original"] = str(original)
            t["octave"] = "half"
            t["bpm_halved"] = round(tempo_of(new_t), 2)
            if a.dry_run:
                continue
            write_beats(half_path, new_t, new_pos)
            t["beats"] = str(half_path)
            np.save(gt_path, labels_from_beats(half_path, feats.shape[0]))
        hist = np.histogram(ratios, bins=[0, 1.25, 1.5, 2, 3, 5, 10, 1000])[0] if ratios else []
        print(f"{set_name}: {counts}; parity ratio histogram (<1.25, <1.5, <2, <3, <5, <10, more): {list(hist)}")
    if not a.dry_run:
        save_json(MANIFEST, manifest)
        print(f"wrote {MANIFEST}")


if __name__ == "__main__":
    main(sys.argv[1:])
