#!/usr/bin/env python3
"""Turn BeatNet+'s published .pt weight sets into the blobs takt4 loads.

HANDOFF §8 Phase 3. The three weight files live in the BeatNet+ repository, which is
not vendored here — clone it somewhere outside git (references/ is excluded for exactly
this) and point the script at the models directory:

    git clone https://github.com/mjhydri/BeatNet-Plus references/beatnet-plus
    git -C references/beatnet-plus checkout bb90eb0a9065b101a4b4c4cb2b2061950266cb4b
    python tools/convert_weights.py references/beatnet-plus/src/BeatNetPlus/models

For each of the three it writes assets/weights/<name>.bin — the parameters, float32,
little-endian, in the order src/core/model/weights.cpp reads them — and <name>.json
recording where they came from. Both are committed; the .pt files are not.

The deliberate deviation from §9, which asks for "3 x RTNeural JSON": these are flat
binary. RTNeural's JSON for one 767,125-parameter set is about 15 MB of text that has
to be parsed at startup, and a generated C++ table of the same numbers is about 11 MB
of source per set. The blob is 3.07 MB, is read once when a model is created — never on
the audio path, which is what §5.2 actually requires — and carries a header the loader
checks, so a mismatched or truncated file is a clean error rather than a wrong model.

Nothing is rewritten on the way through: the numbers land in the file exactly as
PyTorch stores them, gate order included, and src/core/model/ does the transposing
RTNeural needs. One conversion, one place to check it.
"""

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from beatnet_model import (  # noqa: E402
    CONV_FILTERS,
    CONV_FLAT_DIM,
    FEATURE_DIM,
    KERNEL_SIZE,
    NUM_CELLS,
    NUM_CLASSES,
    NUM_LAYERS,
    TOTAL_PARAMS,
    load_state_dict,
)

DEFAULT_OUT_DIR = Path(__file__).resolve().parent.parent / "assets" / "weights"
UPSTREAM = "https://github.com/mjhydri/BeatNet-Plus"
UPSTREAM_COMMIT = "bb90eb0a9065b101a4b4c4cb2b2061950266cb4b"

MAGIC = b"TAKT4WTS"
FORMAT_VERSION = 1
HEADER = struct.Struct("<8sIIIIIIIIII")  # magic, version, 8 dimensions, fnv1a of the payload

# The three published sets, upstream file -> the name takt4 knows it by. The use cases
# are the ones BeatNet+'s README gives, not a guess from the file names.
WEIGHT_SETS = {
    "generic_weights.pt": ("generic", "general-purpose, any level of percussion"),
    "generic_main_weights.pt": ("generic-main", "percussion-heavy material"),
    "af_non_percussive_weights.pt": ("af-non-percussive", "non-percussive, ambient, classical"),
}

# The parameters in the order they are written. Every one of them, once.
PARAMETER_ORDER = (
    ["conv1.weight", "conv1.bias", "linear0.weight", "linear0.bias"]
    + [f"lstm.{kind}_l{layer}" for layer in range(NUM_LAYERS)
       for kind in ("weight_ih", "weight_hh", "bias_ih", "bias_hh")]
    + ["output_linear.weight", "output_linear.bias"]
)


def fnv1a32(data):
    """FNV-1a over the payload. Cheap on both sides and enough to catch a bad file."""
    h = 0x811C9DC5
    for byte in memoryview(data).cast("B"):
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def blob(state):
    """The parameters as one little-endian float32 array, in PARAMETER_ORDER."""
    flat = [state[name].detach().cpu().numpy().astype("<f4", copy=False).reshape(-1)
            for name in PARAMETER_ORDER]
    values = np.concatenate(flat)
    if values.size != TOTAL_PARAMS:
        raise SystemExit(f"wrote {values.size} parameters, expected {TOTAL_PARAMS}")
    payload = values.tobytes()
    header = HEADER.pack(MAGIC, FORMAT_VERSION, FEATURE_DIM, CONV_FILTERS, KERNEL_SIZE,
                         CONV_FLAT_DIM, NUM_CELLS, NUM_LAYERS, NUM_CLASSES,
                         TOTAL_PARAMS, fnv1a32(payload))
    return header + payload


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def convert(source, out_dir, name=None, use_case=None, training=None):
    """Write <name>.bin and <name>.json. Without `name` the source must be one of the
    three published sets; with it, any BeatNet+ branch state_dict — a fine-tune from
    tools/train/finetune.py — and `training` (a dict, typically the run's provenance) is
    recorded in the JSON beside the upstream fields, which still name the architecture
    and the weights it started from."""
    if name is None:
        name, use_case = WEIGHT_SETS[source.name]
    state = load_state_dict(source)
    data = blob(state)

    bin_path = out_dir / f"{name}.bin"
    bin_path.write_bytes(data)
    info = {
        "name": name,
        "use_case": use_case,
        "source": source.name if training is None else str(source),
        "source_bytes": source.stat().st_size,
        "source_sha256": sha256(source),
        "upstream": UPSTREAM,
        "upstream_commit": UPSTREAM_COMMIT,
        "format_version": FORMAT_VERSION,
        "parameters": TOTAL_PARAMS,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "torch": torch.__version__,
        "numpy": np.__version__,
    }
    if training is not None:
        info["training"] = training
    (out_dir / f"{name}.json").write_text(json.dumps(info, indent=2) + "\n",
                                          encoding="utf-8", newline="\n")
    print(f"{source.name} -> {bin_path.name}  {len(data)} bytes, {TOTAL_PARAMS} parameters "
          f"({use_case})")
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("models", type=Path,
                        help="BeatNet+'s src/BeatNetPlus/models directory, one .pt in it, or "
                             "with --name any branch state_dict (a fine-tune's best.pt)")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR,
                        help=f"default: {DEFAULT_OUT_DIR}")
    parser.add_argument("--name", help="the set's name for a .pt that is not one of the three "
                                       "published ones; writes <name>.bin and <name>.json")
    parser.add_argument("--use-case", default="", help="with --name: what the set is for")
    parser.add_argument("--provenance", type=Path,
                        help="with --name: a JSON file recorded under \"training\" in <name>.json")
    args = parser.parse_args()

    if args.name:
        if not args.models.is_file():
            raise SystemExit(f"{args.models} is not a file")
        training = json.loads(args.provenance.read_text(encoding="utf-8")) if args.provenance else {}
        args.out_dir.mkdir(parents=True, exist_ok=True)
        convert(args.models, args.out_dir, name=args.name, use_case=args.use_case, training=training)
        return

    if args.models.is_dir():
        sources = [args.models / name for name in WEIGHT_SETS]
    elif args.models.name in WEIGHT_SETS:
        sources = [args.models]
    else:
        raise SystemExit(f"{args.models} is not one of {sorted(WEIGHT_SETS)} nor a directory of them; "
                         f"pass --name for a fine-tuned set")
    missing = [str(path) for path in sources if not path.is_file()]
    if missing:
        raise SystemExit("not found: " + ", ".join(missing))

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for source in sources:
        convert(source, args.out_dir)


if __name__ == "__main__":
    main()
