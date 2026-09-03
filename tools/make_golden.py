#!/usr/bin/env python3
"""Cut a 10 s excerpt from a track and write the golden feature file for it.

HANDOFF §8 Phase 2: the same audio through madmom and through takt4's C++ front end
must agree to within floating-point tolerance, on at least ten varied real recordings.
The full tracks stay out of git (references/audio/ is excluded); what is committed,
under tests/data/features/, is per excerpt:

    <name>.wav   10 s, 22050 Hz, mono, 16-bit PCM — the exact samples the C++ side reads
    <name>.npy   madmom's (500, 288) float32 features for those exact samples
    <name>.json  where the excerpt came from and which package versions produced it

The excerpt is decoded and resampled by librosa (soundfile + soxr, as BeatNet+ loads
audio), quantised to 16 bits, and the features are computed from the quantised samples
read back from the WAV, so both sides start from identical numbers. The test
tests/features/feature_extractor_test.cpp checks every pair it finds.

    python tools/make_golden.py references/audio/track.flac [--name NAME] [--offset SECONDS]
    python tools/make_golden.py --synthetic

Any format libsndfile decodes works (WAV, FLAC, MP3, OGG, AIFF); AAC/M4A do not. The
default window starts 30 s in, or is centred for tracks shorter than 40 s. --synthetic
writes a deterministic drum-machine signal instead, for a pair that carries no rights.
"""

import argparse
import json
import re
import sys
from pathlib import Path

import librosa
import numpy as np
import scipy
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
from beatnet_features import FEATURE_DIM, HOP_SIZE, SAMPLE_RATE, FeaturePipeline  # noqa: E402
from dump_filterbank import madmom_source  # noqa: E402

DURATION = 10.0
DEFAULT_OFFSET = 30.0
DEFAULT_OUT_DIR = Path(__file__).resolve().parent.parent / "tests" / "data" / "features"
RESAMPLER = "soxr_hq"  # librosa.load's default; spelled out so the sidecar records it


def excerpt_from_file(source, offset):
    total = librosa.get_duration(path=source)
    if total < DURATION:
        raise SystemExit(f"{source} is {total:.1f} s long; {DURATION:.0f} s are needed")
    if offset is None:
        offset = DEFAULT_OFFSET if total >= DEFAULT_OFFSET + DURATION else (total - DURATION) / 2
    if offset + DURATION > total + 1e-6:
        raise SystemExit(f"--offset {offset} + {DURATION:.0f} s runs past the end of {source} ({total:.1f} s)")
    audio, sr = librosa.load(source, sr=SAMPLE_RATE, mono=True, offset=offset, duration=DURATION, res_type=RESAMPLER)
    if sr != SAMPLE_RATE:
        raise SystemExit(f"librosa returned {sr} Hz")
    return audio, offset, total


def synthetic_excerpt():
    """A drum machine at 128 BPM with a bass line and a pad, plus a noise floor. Seeded."""
    rng = np.random.default_rng(20260903)
    n = int(DURATION * SAMPLE_RATE)
    t = np.arange(n) / SAMPLE_RATE
    beat = 60.0 / 128.0
    out = np.zeros(n)
    # Kick on every beat: a pitched sine dropping from 120 Hz to 50 Hz, 150 ms decay.
    for k in range(int(DURATION / beat) + 1):
        start = int(k * beat * SAMPLE_RATE)
        length = min(int(0.3 * SAMPLE_RATE), n - start)
        if length <= 0:
            break
        tt = np.arange(length) / SAMPLE_RATE
        freq = 50.0 + 70.0 * np.exp(-tt / 0.03)
        phase = 2 * np.pi * np.cumsum(freq) / SAMPLE_RATE
        out[start : start + length] += 0.9 * np.sin(phase) * np.exp(-tt / 0.15)
    # Closed hats on the off-beats (odd half-beats), snare-ish noise on beats 2 and 4.
    for k in range(1, int(2 * DURATION / beat) + 1, 2):
        start = int(k * beat / 2 * SAMPLE_RATE)
        length = min(int(0.05 * SAMPLE_RATE), n - start)
        if length <= 0:
            break
        tt = np.arange(length) / SAMPLE_RATE
        out[start : start + length] += 0.25 * rng.standard_normal(length) * np.exp(-tt / 0.012)
    for k in range(1, int(DURATION / beat) + 1, 2):
        start = int(k * beat * SAMPLE_RATE)
        length = min(int(0.2 * SAMPLE_RATE), n - start)
        if length <= 0:
            break
        tt = np.arange(length) / SAMPLE_RATE
        out[start : start + length] += 0.5 * rng.standard_normal(length) * np.exp(-tt / 0.05)
    # Bass line, one note per beat, and a sustained pad.
    notes = [55.0, 55.0, 65.41, 73.42]
    for k in range(int(DURATION / beat) + 1):
        start = int(k * beat * SAMPLE_RATE)
        length = min(int(beat * SAMPLE_RATE), n - start)
        if length <= 0:
            break
        tt = np.arange(length) / SAMPLE_RATE
        f0 = notes[k % len(notes)]
        env = np.minimum(tt / 0.01, 1.0) * np.exp(-tt / 0.4)
        out[start : start + length] += 0.4 * env * (np.sin(2 * np.pi * f0 * tt) + 0.3 * np.sin(2 * np.pi * 2 * f0 * tt))
    pad = sum(np.sin(2 * np.pi * f * t + p) for f, p in ((220.0, 0.0), (277.18, 1.0), (329.63, 2.0), (440.0, 3.0)))
    out += 0.08 * pad
    out += 0.003 * rng.standard_normal(n)
    return (0.8 * out / np.abs(out).max()).astype(np.float32)


def quantise(audio):
    """float32 in [-1, 1) -> int16 the way the C++ reader undoes it: x = q / 32768."""
    q = np.rint(np.clip(audio, -1.0, 32767 / 32768) * 32768.0)
    return q.astype(np.int16)


def rms_dbfs(x):
    rms = float(np.sqrt(np.mean(np.square(x, dtype=np.float64))))
    return 20 * np.log10(rms) if rms > 0 else -np.inf


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", nargs="?", type=Path, help="audio file to cut the excerpt from")
    parser.add_argument("--synthetic", action="store_true", help="generate the built-in test signal instead")
    parser.add_argument("--name", help="base name of the output files (default: from the source name)")
    parser.add_argument("--offset", type=float, help=f"start of the excerpt in seconds (default: {DEFAULT_OFFSET:.0f}, or centred)")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help=f"default: {DEFAULT_OUT_DIR}")
    args = parser.parse_args()
    if args.synthetic == (args.source is not None):
        parser.error("give either a source file or --synthetic")

    if args.synthetic:
        audio, offset, total = synthetic_excerpt(), 0.0, DURATION
        source_name = "synthetic (tools/make_golden.py --synthetic)"
        name = args.name or "synthetic"
    else:
        audio, offset, total = excerpt_from_file(args.source, args.offset)
        source_name = args.source.name
        name = args.name or re.sub(r"[^a-z0-9]+", "-", args.source.stem.lower()).strip("-")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name):
        raise SystemExit(f"--name must be lowercase letters, digits and dashes, got {name!r}")

    expected = int(DURATION * SAMPLE_RATE)
    if len(audio) != expected:
        raise SystemExit(f"got {len(audio)} samples, expected {expected}")
    if expected % HOP_SIZE:
        raise SystemExit("the excerpt is not a whole number of hops")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    wav_path = args.out_dir / f"{name}.wav"
    npy_path = args.out_dir / f"{name}.npy"
    json_path = args.out_dir / f"{name}.json"

    q = quantise(audio)
    sf.write(wav_path, q, SAMPLE_RATE, subtype="PCM_16", format="WAV")
    reread, sr = sf.read(wav_path, dtype="float32", always_2d=False)
    if sr != SAMPLE_RATE or reread.ndim != 1 or not np.array_equal(reread, q.astype(np.float32) / 32768.0):
        raise SystemExit(f"{wav_path} did not read back as the quantised samples")

    feats = FeaturePipeline().features(reread)
    if feats.shape != (expected // HOP_SIZE, FEATURE_DIM):
        raise SystemExit(f"unexpected feature shape {feats.shape}")
    np.save(npy_path, feats)

    info = {
        "source": source_name,
        "offset_seconds": round(offset, 3),
        "source_duration_seconds": round(total, 3),
        "duration_seconds": DURATION,
        "sample_rate": SAMPLE_RATE,
        "samples": expected,
        "frames": int(feats.shape[0]),
        "peak": round(float(np.abs(reread).max()), 6),
        "rms_dbfs": round(rms_dbfs(reread), 2),
        "clipped_samples": int(np.count_nonzero(np.abs(audio) >= 1.0)),
        "decoder": "numpy, generated" if args.synthetic else "librosa.load (soundfile), resampler " + RESAMPLER,
        "madmom": madmom_source(),
        "numpy": np.__version__,
        "scipy": scipy.__version__,
        "librosa": librosa.__version__,
        "soundfile": sf.__version__ + " / libsndfile " + sf.__libsndfile_version__,
    }
    json_path.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8", newline="\n")

    print(f"{name}: {source_name} @ {offset:.1f} s -> {wav_path.name}, {npy_path.name}, {json_path.name}")
    print(f"  peak {info['peak']:.3f}, rms {info['rms_dbfs']:.1f} dBFS, {info['clipped_samples']} clipped, "
          f"{feats.shape[0]} frames, features {feats.min():.3f}..{feats.max():.3f}")


if __name__ == "__main__":
    main()
