"""Paths, set definitions and readers shared by the fine-tune pipeline in this directory.

TRACKING-PROPOSAL.md §7.5, steps 2 to 4: lay the annotated sets out for BeatNet+'s
training code, extract the features it trains on, check every annotation against its
audio, fine-tune from `generic_weights.pt` with Ballroom in the mix, convert. Each stage
is a script that hands the next one files:

    layout.py     references/datasets/<set>/annotations/<id>.beats — every set's own
                  annotation format rewritten in the Ballroom layout prepare_data.py
                  reads (`<seconds> TAB <beat in bar>`, downbeat = 1), beside the audio
                  it belongs to — and WORK/manifest.json: every track, its split, and
                  the flags that keep it out of training (held out, duration mismatch).
    features.py   WORK/features/<set>/<id>.npy, the (frames, 288) log-spectrogram
                  features BeatNet+ trains on, and <id>.gt.npy, the class per frame.
    check.py      WORK/alignment.json: whether the shipped model's activation fires on
                  the annotation's grid, and at what time offset — the check that a
                  fetched audio file is the one that was annotated.
    finetune.py   WORK/runs/<name>/: checkpoints, log.csv, val.json, status.json.

WORK defaults to <repo>/build/training, beside the build trees and on the same volume as
the datasets: an epoch reads a random fifteen-second crop out of every one of ~3,000
tracks. Override with TAKT4_TRAIN_WORK.
"""
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
DATASETS = ROOT / "references" / "datasets"
BALLROOM_AUDIO = ROOT / "references" / "ballroom-audio" / "BallroomData"
BALLROOM_ANNOTATIONS = ROOT / "references" / "ballroom-annotations"
BEATNET_SRC = ROOT / "references" / "beatnet-plus" / "src"
GENERIC_WEIGHTS = BEATNET_SRC / "BeatNetPlus" / "models" / "generic_weights.pt"

WORK = Path(os.environ.get("TAKT4_TRAIN_WORK", ROOT / "build" / "training"))
FEATURES = WORK / "features"
RUNS = WORK / "runs"
MANIFEST = WORK / "manifest.json"
ALIGNMENT = WORK / "alignment.json"

#: The network's frame rate and the front end's parameters, as BeatNet+'s configs and
#: tools/beatnet_features.py have them.
FPS = 50
SAMPLE_RATE = 22050
HOP = 441
NUM_BANDS = 144          # log-filtered magnitudes, then their positive differences
FEATURE_DIM = 2 * NUM_BANDS

#: Set name -> (directory under references/datasets, audio directory, annotations directory
#: the derived .beats go to). Ballroom's two directories are junctions layout.py creates.
SETS = {
    "raveform": ("raveform", "audio", "annotations"),
    "osu2beat": ("osu2mir", "audio", "annotations"),
    "harmonix": ("harmonixset", "audio", "annotations"),
    "ballroom": ("ballroom", "audio", "annotations"),
    # The operator's own library, pseudo-labelled by Beat This! (tools/train/distil.py).
    "library": ("library", "audio", "annotations"),
    # The operator's own annotations, tapped in (tools/annotate.py --dataset): the one set
    # whose labels are the operator's convention and nobody else's.
    "operator": ("operator", "audio", "annotations"),
}

#: The sets that are the operator's, laid out by their own tools rather than fetched;
#: layout.py takes them by name only.
OWN_SETS = ("library", "operator")

#: Tracks of `references/audio` (the proposal's 23, and the operator's own material) that
#: turn up in a training set. Held out, so the gate measures generalisation.
HELD_OUT = {
    "raveform": {"LiJrhQC8oqU": "808 State - In Yer Face (Bicep Remix): references/audio"},
}


def add_tools_path():
    """Make tools/ and BeatNet+'s package importable."""
    for p in (str(ROOT / "tools"), str(BEATNET_SRC)):
        if p not in sys.path:
            sys.path.insert(0, p)


def read_beats(path):
    """(times, beat-in-bar) from a Ballroom-layout file; whitespace or tab separated."""
    rows = np.loadtxt(path, ndmin=2)
    if rows.size == 0:
        return np.zeros(0), np.zeros(0, dtype=int)
    times = rows[:, 0].astype(float)
    positions = rows[:, 1].astype(int) if rows.shape[1] > 1 else np.zeros(len(times), dtype=int)
    order = np.argsort(times, kind="stable")
    return times[order], positions[order]


def write_beats(path, times, positions):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for t, p in zip(times, positions):
            f.write(f"{t:.6f}\t{int(p)}\n")


def labels_from_beats(path, frames):
    """The class per frame (0 beat, 1 downbeat, 2 neither) that a `.beats` file makes over
    `frames` frames: prepare_data.py's build_ground_truth, imported, as features.py uses it.

    Every script that writes a `.gt.npy` writes this, from the file it has just written —
    never from the times it had in memory. The two are not the same: a beat shifted from
    1.12 s to 1.1400000000000001 s is on frame 57, and the 1.140000 that reaches the file is
    on frame 56. Seventeen osu2beat tracks carried exactly that difference from
    shift_labels.py (2026-09-26)."""
    add_tools_path()
    from BeatNetPlus.prepare_data import build_ground_truth
    times, positions = read_beats(path)
    gt = build_ground_truth(times[positions != 1], times[positions == 1], frames, SAMPLE_RATE, HOP)
    return np.argmax(gt, axis=0).astype(np.int8)


def label_mismatches(tracks, jobs=8):
    """[(set, id, why)] for every (set, id, beats path) whose `.gt.npy` is not exactly the
    labels its beats make — or is missing, or has not the features' length.

    finetune.py trains from the `.gt.npy` and validates against the beats, so where the
    two disagree a run learns one labelling and is selected on another. That is how v4 and
    v5 were run (TRACKING-PROPOSAL.md §7.16): 254 library tracks trained at half time and
    scored against their annotations at full time."""
    from concurrent.futures import ThreadPoolExecutor

    def one(item):
        set_name, track_id, beats = item
        feat_path, gt_path = feature_paths(set_name, track_id)
        if not gt_path.exists():
            return set_name, track_id, "no .gt.npy"
        stored = np.load(gt_path)
        if feat_path.exists():
            frames = int(np.load(feat_path, mmap_mode="r").shape[0])
            if frames != len(stored):
                return set_name, track_id, f".gt.npy has {len(stored)} frames, the features {frames}"
        made = labels_from_beats(beats, len(stored))
        if np.array_equal(stored, made):
            return None
        counts = lambda c: (int((c == 0).sum()) + int((c == 1).sum()), int((c == 1).sum()))
        (sb, sd), (mb, md) = counts(stored), counts(made)
        return set_name, track_id, (f".gt.npy has {sb} beats ({sd} downbeats), {Path(beats).name} "
                                    f"makes {mb} ({md}); {int((stored != made).sum())} frames differ")

    add_tools_path()
    import BeatNetPlus.prepare_data  # noqa: F401 - imported once here, not first in eight threads
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        return [r for r in pool.map(one, list(tracks)) if r is not None]


def load_manifest():
    if not MANIFEST.exists():
        raise SystemExit(f"{MANIFEST} not found; run tools/train/layout.py first")
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f)


def save_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")


def load_json(path, default=None):
    if not Path(path).exists():
        return default
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def feature_paths(set_name, track_id):
    d = FEATURES / set_name
    return d / f"{track_id}.npy", d / f"{track_id}.gt.npy"


def lower_priority():
    """Run this process (and the workers it spawns, which inherit the class) below the
    desktop's priority, so a training run never makes the machine feel busy."""
    if os.name != "nt":
        try:
            os.nice(10)
        except OSError:
            pass
        return
    import ctypes
    BELOW_NORMAL_PRIORITY_CLASS = 0x4000
    kernel32 = ctypes.windll.kernel32
    kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)


def _refeval_common():
    """tools/refeval/common.py, loaded under its own module name since both directories have
    a `common`."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("refeval_common", ROOT / "tools" / "refeval" / "common.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def cli_path():
    """The takt4-cli to measure with: tools/refeval/common.py's rule (TAKT4_CLI, else the
    local Release build)."""
    return _refeval_common().cli_path()


def cli_identity(cli):
    """tools/refeval/common.py's: the CLI's path, SHA-256 and `--version`."""
    return _refeval_common().cli_identity(cli)


def tempo_of(times):
    gaps = np.diff(np.asarray(times, float))
    gaps = gaps[gaps > 0]
    return float(60.0 / np.median(gaps)) if len(gaps) else 0.0


def meter_of(positions):
    """The most common number of beats between downbeats, or 0."""
    ones = np.where(np.asarray(positions) == 1)[0]
    if len(ones) < 2:
        return 0
    values, counts = np.unique(np.diff(ones), return_counts=True)
    return int(values[np.argmax(counts)])
