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
    python tools/make_golden.py references/audio/track.flac --auto
    python tools/make_golden.py --synthetic
    python tools/make_golden.py --regenerate [NAME ...]    # the .npy again, from the committed .wav

Any format libsndfile decodes works (WAV, FLAC, MP3, OGG, AIFF); AAC/M4A do not. The
default window starts 30 s in, or is centred for tracks shorter than 40 s. --auto reads
the whole track and picks the window instead (see pick_offset); --synthetic writes a
deterministic drum-machine signal, for a pair that carries no rights.
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

# --auto scoring. A window is wanted that sounds like the track: a clear pulse, at the
# loudness the track spends most of its time at, with no hole in it. The constants are
# taste, not theory — they are here, named, so a pick can be argued with.
LEVEL_HOP = SAMPLE_RATE // 10  # 100 ms level frames, ten to the second
BLOCK_HOP = SAMPLE_RATE  # 1 s level frames, for finding holes
ONSET_HOP = 512  # librosa's onset envelope, 43 frames a second
CANDIDATE_STEP = 1.0  # candidate offsets are whole seconds
PULSE_PERIODS = (0.3, 1.5)  # lags searched for a pulse: 200 down to 40 BPM
LEVEL_SCALE = 6.0  # dB the window's median may sit off the track's usual level for e^-1
DIP_TOLERANCE = 10.0  # dB a single second may fall below that level for free
DIP_SCALE = 8.0  # dB of further dip for another factor of e^-1
END_MARGIN = 1.0  # seconds left untouched at the end; some MP3 seeks read short there


def level_dbfs(audio, hop):
    """Frame level in dBFS, frames starting at 0, hop long, no centring or padding."""
    rms = librosa.feature.rms(y=audio, frame_length=hop, hop_length=hop, center=False)[0]
    return 20.0 * np.log10(np.maximum(rms, 1e-10))


def modal_level(level):
    """The track's most common loudness: the peak of a 1 dB histogram of its frames.

    Frames more than 60 dB below the loudest are dropped first, so leading digital
    silence — or a fade — cannot become the mode of a track that is mostly music.
    """
    audible = level[level > level.max() - 60.0]
    if audible.size == 0:
        raise SystemExit("the track is silent")
    edges = np.arange(np.floor(audible.min()), np.ceil(audible.max()) + 1.0)
    if edges.size < 2:
        return float(audible.mean())
    counts = np.histogram(audible, bins=edges)[0].astype(np.float64)
    smoothed = np.convolve(counts, np.ones(3) / 3.0, mode="same")  # 1 dB bins are noisy
    peak = int(np.argmax(smoothed))
    return float((edges[peak] + edges[peak + 1]) / 2.0)


def pulse_clarity(onset_envelope):
    """How strongly the window pulses: the top of its onset envelope's autocorrelation.

    Biased normalisation (every lag divided by lag zero), so a periodicity that only
    fits into the window twice is not flattered. Roughly 0 for unmetred sound, 0.2-0.6
    for a plain drum pattern.
    """
    centred = onset_envelope - onset_envelope.mean()
    energy = float(np.dot(centred, centred))
    if energy <= 0.0:
        return 0.0
    correlation = np.correlate(centred, centred, mode="full")[centred.size - 1 :] / energy
    low = int(round(PULSE_PERIODS[0] * SAMPLE_RATE / ONSET_HOP))
    high = min(correlation.size - 1, int(round(PULSE_PERIODS[1] * SAMPLE_RATE / ONSET_HOP)))
    return float(correlation[low : high + 1].max()) if high > low else 0.0


def window_at(offset, fine, blocks, envelope, usual):
    """Score the 10 s window starting at offset. Higher is a better excerpt."""
    window_fine = fine[int(offset * SAMPLE_RATE / LEVEL_HOP) : int((offset + DURATION) * SAMPLE_RATE / LEVEL_HOP)]
    window_blocks = blocks[int(offset * SAMPLE_RATE / BLOCK_HOP) : int((offset + DURATION) * SAMPLE_RATE / BLOCK_HOP)]
    window_env = envelope[int(offset * SAMPLE_RATE / ONSET_HOP) : int((offset + DURATION) * SAMPLE_RATE / ONSET_HOP)]
    if window_fine.size == 0 or window_blocks.size == 0 or window_env.size < 2:
        return None
    clarity = pulse_clarity(window_env)
    median = float(np.median(window_fine))
    quietest = float(window_blocks.min())
    dip = max(0.0, usual - quietest - DIP_TOLERANCE)
    score = clarity * np.exp(-(((median - usual) / LEVEL_SCALE) ** 2)) * np.exp(-((dip / DIP_SCALE) ** 2))
    return {
        "offset": offset,
        "score": float(score),
        "pulse_clarity": round(clarity, 3),
        "level_dbfs": round(median, 1),
        "track_usual_level_dbfs": round(usual, 1),
        "quietest_second_dbfs": round(quietest, 1),
    }


def pick_offset(source, report_top=0):
    """Choose where to cut, by scoring every whole-second window of the whole track.

    A window is wanted that sounds like the track: pulse clarity, discounted for sitting
    off the track's usual loudness and for containing a second much quieter than that —
    which is what an intro, an outro or a breakdown looks like from here. Returns the
    winning window's scores, offset included.
    """
    audio, _ = librosa.load(source, sr=SAMPLE_RATE, mono=True, res_type=RESAMPLER)
    decoded = audio.size / SAMPLE_RATE  # not get_duration: MP3 headers over-report
    last = decoded - DURATION - END_MARGIN
    if last < 0.0:
        raise SystemExit(f"{source} decodes to {decoded:.1f} s; --auto needs {DURATION + END_MARGIN:.0f} s")

    fine = level_dbfs(audio, LEVEL_HOP)
    blocks = level_dbfs(audio, BLOCK_HOP)
    usual = modal_level(fine)
    envelope = librosa.onset.onset_strength(y=audio, sr=SAMPLE_RATE, hop_length=ONSET_HOP)

    scored = [window_at(step * CANDIDATE_STEP, fine, blocks, envelope, usual) for step in range(int(last / CANDIDATE_STEP) + 1)]
    scored = [window for window in scored if window is not None]
    if not scored:
        raise SystemExit(f"no window of {source} could be scored")
    scored.sort(key=lambda window: (-window["score"], window["offset"]))  # ties go to the earlier window

    if report_top:
        print(f"  {decoded:.0f} s decoded, usual level {usual:.1f} dBFS; best {min(report_top, len(scored))} of {len(scored)} windows:")
        for window in scored[:report_top]:
            print(f"    {window['offset']:6.0f} s  score {window['score']:.3f}  pulse {window['pulse_clarity']:.3f}"
                  f"  level {window['level_dbfs']:+6.1f} dBFS ({window['level_dbfs'] - usual:+.1f})"
                  f"  quietest second {window['quietest_second_dbfs']:+6.1f}")
    return scored[0]


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


def versions():
    """The packages the features depend on, as a sidecar records them."""
    return {
        "madmom": madmom_source(),
        "numpy": np.__version__,
        "scipy": scipy.__version__,
        "librosa": librosa.__version__,
        "soundfile": sf.__version__ + " / libsndfile " + sf.__libsndfile_version__,
    }


def regenerate(out_dir, names):
    """Each excerpt's `.npy` again from its committed `.wav`, which is the exact input, and
    the sidecar's package versions with it; where the excerpt came from is kept.

    Doing this by passing the `.wav` to make_golden.py as a source, which the README used to
    suggest, reproduced the data bit for bit and wrote a new sidecar describing a ten-second
    file cut at 0 s — the track, the offset and --auto's measurements lost (the 2026-09-25
    audit's P11)."""
    names = names or sorted(p.stem for p in out_dir.glob("*.wav"))
    for name in names:
        wav_path, npy_path, json_path = (out_dir / f"{name}{ext}" for ext in (".wav", ".npy", ".json"))
        if not wav_path.exists() or not json_path.exists():
            raise SystemExit(f"{name}: no {wav_path.name} and {json_path.name} under {out_dir}")
        samples, sr = sf.read(wav_path, dtype="float32", always_2d=False)
        if sr != SAMPLE_RATE or samples.ndim != 1:
            raise SystemExit(f"{wav_path}: {sr} Hz, {samples.ndim} channels; not an excerpt")
        feats = FeaturePipeline().features(samples)
        if feats.shape != (len(samples) // HOP_SIZE, FEATURE_DIM):
            raise SystemExit(f"{name}: unexpected feature shape {feats.shape}")
        info = json.loads(json_path.read_text(encoding="utf-8"))
        before = np.load(npy_path) if npy_path.exists() else None
        np.save(npy_path, feats)
        info.update(versions())
        json_path.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8", newline="\n")
        same = before is not None and before.shape == feats.shape and np.array_equal(before, feats)
        print(f"{name}: {info.get('source')} @ {info.get('offset_seconds')} s -> {npy_path.name} "
              + ("(unchanged)" if same else "(CHANGED)" if before is not None else "(new)"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", nargs="?", type=Path, help="audio file to cut the excerpt from")
    parser.add_argument("--synthetic", action="store_true", help="generate the built-in test signal instead")
    parser.add_argument("--regenerate", nargs="*", metavar="NAME", default=None,
                        help="recompute the named excerpts' .npy (all of them, given no name) from "
                             "their committed .wav, keeping each sidecar's provenance")
    parser.add_argument("--name", help="base name of the output files (default: from the source name)")
    parser.add_argument("--offset", type=float, help=f"start of the excerpt in seconds (default: {DEFAULT_OFFSET:.0f}, or centred)")
    parser.add_argument("--auto", action="store_true", help="choose the offset by analysing the whole track")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help=f"default: {DEFAULT_OUT_DIR}")
    args = parser.parse_args()
    if args.regenerate is not None:
        if args.source is not None or args.synthetic or args.auto or args.offset is not None or args.name:
            parser.error("--regenerate takes excerpt names and nothing else")
        regenerate(args.out_dir, args.regenerate)
        return
    if args.synthetic == (args.source is not None):
        parser.error("give either a source file or --synthetic")
    if args.auto and (args.offset is not None or args.synthetic):
        parser.error("--auto takes a source file and no --offset")

    selection = None
    if args.synthetic:
        audio, offset, total = synthetic_excerpt(), 0.0, DURATION
        source_name = "synthetic (tools/make_golden.py --synthetic)"
        offset_source = "synthetic"
        name = args.name or "synthetic"
    else:
        chosen = args.offset
        offset_source = "explicit" if chosen is not None else "default"
        if args.auto:
            offset_source = "auto"
            selection = pick_offset(args.source, report_top=5)
            chosen = selection.pop("offset")
            selection["score"] = round(selection["score"], 3)
        audio, offset, total = excerpt_from_file(args.source, chosen)
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
        "offset_source": offset_source,
        "source_duration_seconds": round(total, 3),
        "duration_seconds": DURATION,
        "sample_rate": SAMPLE_RATE,
        "samples": expected,
        "frames": int(feats.shape[0]),
        "peak": round(float(np.abs(reread).max()), 6),
        "rms_dbfs": round(rms_dbfs(reread), 2),
        "clipped_samples": int(np.count_nonzero(np.abs(audio) >= 1.0)),
        "decoder": "numpy, generated" if args.synthetic else "librosa.load (soundfile), resampler " + RESAMPLER,
        **versions(),
    }
    if selection is not None:
        # What --auto measured on the whole track when it settled on this window.
        info["offset_selection"] = selection
    json_path.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8", newline="\n")

    print(f"{name}: {source_name} @ {offset:.1f} s -> {wav_path.name}, {npy_path.name}, {json_path.name}")
    print(f"  peak {info['peak']:.3f}, rms {info['rms_dbfs']:.1f} dBFS, {info['clipped_samples']} clipped, "
          f"{feats.shape[0]} frames, features {feats.min():.3f}..{feats.max():.3f}")


if __name__ == "__main__":
    main()
