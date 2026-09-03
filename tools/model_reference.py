#!/usr/bin/env python3
"""Record what PyTorch's BeatNet+ makes of every golden excerpt, once and for all.

HANDOFF §8 Phase 3's gate is "activation traces match PyTorch to tolerance on the
Phase 2 test set". This runs the real network over the committed features of that test
set and writes the answers into tests/data/model/, so tests/model/ can check the C++
against them forever without torch, without the .pt files, and without Python.

    python tools/convert_weights.py references/beatnet-plus/src/BeatNetPlus/models
    python tools/model_reference.py

The input is tests/data/features/<name>.npy — madmom's own (500, 288) features, not
anything C++ produced. That keeps this a test of the model alone: a Phase 2 regression
fails the feature test, not this one.

Per weight set, per excerpt, tests/data/model/<set>/<name>.npy holds (frames, 6)
float32:

    columns 0..2   the logits the network emits, for beat, downbeat and non-beat
    columns 3..5   softmax over those three, which is what the tracker consumes

Both, because the softmax squashes differences: logits are the strict comparison, and
the probabilities are what actually gets used. tests/data/model/<set>.json records the
versions and a checksum per excerpt.

The trace is produced by BeatNet+'s 'online' path — the whole sequence through the LSTM
from a zero state — because that is a single, unambiguous reference. takt4 runs the
network a frame at a time instead, so the two are checked against each other here on
every excerpt before anything is written.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from beatnet_model import FEATURE_DIM, NUM_CLASSES, load_branch  # noqa: E402
from convert_weights import UPSTREAM_COMMIT, WEIGHT_SETS  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FEATURES_DIR = ROOT / "tests" / "data" / "features"
DEFAULT_OUT_DIR = ROOT / "tests" / "data" / "model"

# How far the frame-by-frame streaming path may differ from the whole-sequence one.
# They are the same arithmetic in a different order, so this is float32 rounding only.
STREAMING_TOLERANCE = 1e-5


def excerpt_features(path):
    feats = np.load(path)
    if feats.ndim != 2 or feats.shape[1] != FEATURE_DIM or feats.dtype != np.float32:
        raise SystemExit(f"{path}: expected float32 (frames, {FEATURE_DIM}), got {feats.dtype}{feats.shape}")
    return feats


def trace(model, feats):
    """(frames, 6) float32: the logits, then the softmax of them."""
    x = torch.from_numpy(feats).unsqueeze(0)
    with torch.no_grad():
        logits = model.inference_forward(x)[0]          # (3, frames)
        probs = torch.nn.functional.softmax(logits, dim=0)
    out = torch.cat((logits, probs)).transpose(0, 1)    # (frames, 6)
    return np.ascontiguousarray(out.numpy(), dtype=np.float32)


def streaming_logits(model, feats):
    """The same run one frame at a time, carrying the LSTM state — takt4's path."""
    model.reset_hidden()
    out = np.empty((feats.shape[0], NUM_CLASSES), dtype=np.float32)
    with torch.no_grad():
        for i, frame in enumerate(feats):
            x = torch.from_numpy(frame).unsqueeze(0).unsqueeze(0)  # (1, 1, 288)
            out[i] = model(x)[0, :, 0].numpy()
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--models", type=Path, default=ROOT / "references" / "beatnet-plus" / "src" / "BeatNetPlus" / "models",
                        help="BeatNet+'s models directory, holding the three .pt files")
    parser.add_argument("--set", dest="sets", action="append", choices=sorted(n for n, _ in WEIGHT_SETS.values()),
                        help="only this weight set (repeatable; default: all three)")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help=f"default: {DEFAULT_OUT_DIR}")
    args = parser.parse_args()

    excerpts = sorted(FEATURES_DIR.glob("*.npy"))
    if not excerpts:
        raise SystemExit(f"no golden features in {FEATURES_DIR}")

    wanted = set(args.sets) if args.sets else None
    for source_name, (set_name, use_case) in WEIGHT_SETS.items():
        if wanted is not None and set_name not in wanted:
            continue
        source = args.models / source_name
        if not source.is_file():
            raise SystemExit(f"{source} not found; see tools/convert_weights.py for where to get it")
        model = load_branch(source)

        out_dir = args.out_dir / set_name
        out_dir.mkdir(parents=True, exist_ok=True)
        traces = {}
        worst_streaming = 0.0
        for path in excerpts:
            feats = excerpt_features(path)
            values = trace(model, feats)
            streaming = streaming_logits(model, feats)
            drift = float(np.abs(streaming.astype(np.float64) - values[:, :NUM_CLASSES].astype(np.float64)).max())
            worst_streaming = max(worst_streaming, drift)
            if drift > STREAMING_TOLERANCE:
                raise SystemExit(f"{set_name}/{path.stem}: frame-by-frame and whole-sequence "
                                 f"differ by {drift:.3e}, over {STREAMING_TOLERANCE:.0e}")
            np.save(out_dir / path.name, values)
            traces[path.stem] = {
                "frames": int(values.shape[0]),
                "sha256": hashlib.sha256((out_dir / path.name).read_bytes()).hexdigest(),
                "logit_range": [round(float(values[:, :NUM_CLASSES].min()), 4),
                                round(float(values[:, :NUM_CLASSES].max()), 4)],
                "max_beat_probability": round(float(values[:, NUM_CLASSES].max()), 4),
                "max_downbeat_probability": round(float(values[:, NUM_CLASSES + 1].max()), 4),
                "streaming_vs_sequence": float(f"{drift:.3e}"),
            }

        info = {
            "weights": set_name,
            "use_case": use_case,
            "source": source_name,
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "upstream_commit": UPSTREAM_COMMIT,
            "input": "tests/data/features/<name>.npy, madmom's features for the same excerpt",
            "columns": "0..2 logits (beat, downbeat, non-beat), 3..5 softmax of them",
            "worst_streaming_vs_sequence": float(f"{worst_streaming:.3e}"),
            "torch": torch.__version__,
            "numpy": np.__version__,
            "excerpts": traces,
        }
        (args.out_dir / f"{set_name}.json").write_text(json.dumps(info, indent=2) + "\n",
                                                       encoding="utf-8", newline="\n")
        print(f"{set_name}: {len(traces)} excerpts -> {out_dir}, "
              f"frame-by-frame within {worst_streaming:.2e} of the whole sequence")


if __name__ == "__main__":
    main()
