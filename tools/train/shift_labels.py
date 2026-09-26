"""Move one set's beat labels by a fixed number of frames, so every set marks the beat
where the others do.

    python tools/train/shift_labels.py osu2beat 1        # one frame (20 ms) later
    python tools/train/shift_labels.py osu2beat 0        # back to the annotation

Measured against the pretrained model's activation peaks (TRACKING-PROPOSAL.md §7.8),
Raveform's and Ballroom's labels sit on the same frame (per-track median offset 0 ms,
means +6 and +9), osu2beat2025's one frame earlier (+20 ms median, +24 mean): rhythm-game
timing points mark the onset itself, the other sets' annotators a frame after. A model
fine-tuned on that mix learns to fire earlier, and on material with soft onsets — slow
Waltzes — the shipped decoder's beats came out 50–70 ms before the annotation (§7.9).

Writes `<id>.shifted.beats` beside the annotation, points the manifest's `beats` and
`beats_original` at it (`beats_annotated` keeps the true original, so octave.py, which
starts from `beats_original`, halves the shifted file), and rebuilds `.gt.npy`. Run
octave.py again afterwards for the halved tracks.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (FPS, MANIFEST, feature_paths, labels_from_beats, load_manifest,  # noqa: E402
                    read_beats, save_json, write_beats)


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    set_name, frames = argv[0], int(argv[1])
    manifest = load_manifest()
    info = manifest["sets"][set_name]
    done = 0
    for t in info["tracks"]:
        annotated = Path(t.get("beats_annotated", t.get("beats_original", t["beats"])))
        times, positions = read_beats(annotated)
        feat_path, gt_path = feature_paths(set_name, t["id"])
        if not feat_path.exists():
            continue
        n_frames = int(np.load(feat_path, mmap_mode="r").shape[0])
        if frames == 0:
            t["beats"] = t["beats_original"] = str(annotated)
            t.pop("beats_annotated", None)
            t.pop("label_shift_frames", None)
        else:
            shifted = annotated.with_name(annotated.stem.replace(".shifted", "") + ".shifted.beats")
            write_beats(shifted, times + frames / FPS, positions)
            t["beats_annotated"] = str(annotated)
            t["beats"] = t["beats_original"] = str(shifted)
            t["label_shift_frames"] = frames
        # Any earlier halving is undone here; octave.py redoes it from the shifted file.
        t.pop("octave", None)
        # From the file, not from `times + frames / FPS`: a beat that lands exactly on a
        # frame boundary can fall on either side of it (common.labels_from_beats).
        np.save(gt_path, labels_from_beats(t["beats"], n_frames))
        done += 1
    save_json(MANIFEST, manifest)
    print(f"{set_name}: {done} tracks shifted by {frames} frame(s); wrote {MANIFEST}")


if __name__ == "__main__":
    main(sys.argv[1:])
