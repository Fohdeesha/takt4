#!/usr/bin/env python3
"""Measure takt4's beat and downbeat F-measure against an annotated dataset.

HANDOFF §8 Phase 4's last item: "offline evaluation against annotated audio; compare F1
with §A.1's published numbers" — BeatNet+'s 80.62 beat and 56.51 downbeat F-measure on
GTZAN at a 70 ms tolerance. That is the only number that can settle Q4, the fate of the
DSP fallback.

This runs the real thing: every file goes through `takt4-cli track`, so what is measured
is the shipped C++ front end, model, particle filter and tempo state machine, not a
Python approximation of them. Scoring is mir_eval's, which is the reference
implementation the published figures are computed with — rolling our own F-measure
would produce a number that is not comparable, which is the whole point of the exercise.

    python tools/evaluate.py path/to/dataset --annotations path/to/annotations

No dataset is vendored: they are large, and their licences are their own. Layouts that
work as they come:

| Dataset | Audio | Annotations |
|---|---|---|
| Ballroom | `BallroomData/<genre>/*.wav` | `BallroomAnnotations/*.beats` |
| SMC MIREX | `SMC_MIREX_Audio/*.wav` | `SMC_MIREX_Annotations/*.txt` |
| GTZAN | `genres/<genre>/*.wav` | `gtzan_tempo_beat/beats/*.beats` |

An annotation is matched to its audio by file stem, searched for recursively, and may be
one column of times or two columns of "time <TAB> beat number in the bar". Downbeat
F-measure needs the second column; without it only the beat numbers are reported.

**The octave fold is off by default.** §5.5's fold is an operator's tool — it needs a
tempo range nobody evaluating a tracker is allowed to assume — and the published numbers
are measured without one. `--bpm LO-HI` turns it on, which is worth doing separately to
see what it is worth on material whose range is known.
"""

import argparse
import concurrent.futures
import json
import platform
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
AUDIO_SUFFIXES = (".wav", ".flac", ".mp3", ".ogg", ".m4a", ".au")
ANNOTATION_SUFFIXES = (".beats", ".txt", ".beats.txt", ".csv", ".onsets")
SAMPLE_RATE = 22050


def find_cli(given):
    """The takt4-cli to measure. An unoptimised build would be a different tracker only
    in speed, but it would take hours, so a Debug path is refused rather than run."""
    if given is not None:
        path = Path(given)
        if not path.is_file():
            raise SystemExit(f"{path}: not found")
        return path
    candidates = [
        Path("C:/build/takt4/windows-core/bin/Release/takt4-cli.exe"),
        ROOT / "build" / "windows-core" / "bin" / "Release" / "takt4-cli.exe",
        ROOT / "build" / "linux-core" / "bin" / "takt4-cli",
        ROOT / "build" / "macos-core" / "bin" / "takt4-cli",
    ]
    for path in candidates:
        if path.is_file():
            return path
    raise SystemExit("no takt4-cli found; pass --cli PATH. Looked in:\n  " +
                     "\n  ".join(str(p) for p in candidates))


def read_annotation(path):
    """(times, beat numbers). The numbers are zeros when the file has only one column."""
    times, numbers = [], []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip().replace(",", " ")
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        try:
            times.append(float(parts[0]))
        except ValueError:
            continue  # a header line, or something that is not an annotation
        numbers.append(int(float(parts[1])) if len(parts) > 1 else 0)
    order = np.argsort(times)
    return np.asarray(times, dtype=float)[order], np.asarray(numbers, dtype=int)[order]


def index_annotations(directory):
    """Every annotation under `directory`, by file stem. A stem seen twice is an error:
    silently picking one of them would make the score depend on directory order."""
    found = {}
    for path in sorted(Path(directory).rglob("*")):
        if not path.is_file():
            continue
        name = path.name.lower()
        if not any(name.endswith(suffix) for suffix in ANNOTATION_SUFFIXES):
            continue
        stem = path.name
        for suffix in sorted(ANNOTATION_SUFFIXES, key=len, reverse=True):
            if stem.lower().endswith(suffix):
                stem = stem[: -len(suffix)]
                break
        stem = stem.lower()
        if stem in found and found[stem] != path:
            raise SystemExit(f"two annotations for '{stem}': {found[stem]} and {path}")
        found[stem] = path
    return found


def track(cli, audio, options, scratch):
    """Decode one file to what takt4-cli reads, run it, and return its beats."""
    import librosa
    import soundfile as sf

    samples, _ = librosa.load(str(audio), sr=SAMPLE_RATE, mono=True)
    wav = scratch / (audio.stem + ".wav")
    sf.write(str(wav), samples.astype(np.float32), SAMPLE_RATE, subtype="FLOAT")
    beats = scratch / (audio.stem + ".beats")

    command = [str(cli), "track", str(wav), "--out", str(beats),
               "--bpm", options.bpm, "--weights", options.weights,
               "--seed", str(options.seed), "--confidence", str(options.confidence)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{audio.name}: takt4-cli exited {result.returncode}: "
                           f"{result.stderr.strip() or result.stdout.strip()[:200]}")
    times, numbers = read_annotation(beats)
    wav.unlink(missing_ok=True)
    beats.unlink(missing_ok=True)
    return times, numbers, len(samples) / SAMPLE_RATE


def score(reference_times, reference_numbers, estimate_times, estimate_numbers):
    """mir_eval's beat metrics, plus the downbeat F-measure where the annotation allows.

    `mir_eval.beat.evaluate` trims the first five seconds off both sides itself, which is
    the convention the published numbers follow.
    """
    import mir_eval

    scores = dict(mir_eval.beat.evaluate(reference_times, estimate_times))
    # Downbeats: the annotation's beat 1 against ours. Trimmed the same way, and only
    # when the annotation actually carries bar positions.
    if np.any(reference_numbers == 1):
        reference_downbeats = mir_eval.beat.trim_beats(reference_times[reference_numbers == 1])
        estimate_downbeats = mir_eval.beat.trim_beats(estimate_times[estimate_numbers == 1])
        if len(reference_downbeats) > 0:
            scores["Downbeat F-measure"] = mir_eval.beat.f_measure(reference_downbeats,
                                                                   estimate_downbeats)
    return scores


def evaluate_one(cli, audio, annotation, options):
    with tempfile.TemporaryDirectory(prefix="takt4-eval-") as scratch:
        estimate_times, estimate_numbers, seconds = track(cli, audio, options, Path(scratch))
    reference_times, reference_numbers = read_annotation(annotation)
    if len(reference_times) < 2:
        raise RuntimeError(f"{annotation.name}: fewer than two annotated beats")
    result = score(reference_times, reference_numbers, estimate_times, estimate_numbers)
    result["_seconds"] = seconds
    result["_reference_beats"] = int(len(reference_times))
    result["_estimated_beats"] = int(len(estimate_times))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dataset", type=Path, help="directory of annotated audio")
    parser.add_argument("--annotations", type=Path,
                        help="where the annotations are; default: the dataset directory")
    parser.add_argument("--cli", help="takt4-cli to measure; default: the local build")
    parser.add_argument("--weights", default="generic",
                        choices=["generic", "generic-main", "af-non-percussive"])
    parser.add_argument("--bpm", default="off",
                        help='octave-fold window, "off" (the default) or LO-HI')
    parser.add_argument("--confidence", type=float, default=0.15)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--limit", type=int, help="stop after this many files")
    parser.add_argument("--jobs", type=int, default=1,
                        help="files in parallel; each one is a whole takt4-cli run")
    parser.add_argument("--report", type=Path, help="write the per-file scores here as JSON")
    options = parser.parse_args()

    cli = find_cli(options.cli)
    audio_files = sorted(path for path in options.dataset.rglob("*")
                         if path.is_file() and path.suffix.lower() in AUDIO_SUFFIXES)
    if not audio_files:
        raise SystemExit(f"no audio under {options.dataset}")
    annotations = index_annotations(options.annotations or options.dataset)
    if not annotations:
        raise SystemExit(f"no annotations under {options.annotations or options.dataset}")

    pairs = []
    unmatched = []
    for audio in audio_files:
        annotation = annotations.get(audio.stem.lower())
        if annotation is None:
            unmatched.append(audio.name)
        else:
            pairs.append((audio, annotation))
    if options.limit:
        pairs = pairs[: options.limit]
    if not pairs:
        raise SystemExit("no audio file matched an annotation by name")

    print(f"{cli}\n{len(pairs)} annotated files"
          + (f", {len(unmatched)} without an annotation" if unmatched else "")
          + f", fold {options.bpm}, weights {options.weights}\n")

    results = {}
    failures = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, options.jobs)) as pool:
        futures = {pool.submit(evaluate_one, cli, audio, annotation, options): audio
                   for audio, annotation in pairs}
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            audio = futures[future]
            try:
                results[audio.stem] = future.result()
            except Exception as error:  # noqa: BLE001 - one bad file must not stop the run
                failures[audio.stem] = str(error)
            if sys.stdout.isatty():
                print(f"\r  {done}/{len(pairs)}", end="", flush=True)
    if sys.stdout.isatty():
        print("\r" + " " * 24 + "\r", end="")

    if not results:
        raise SystemExit("every file failed:\n  " +
                         "\n  ".join(f"{k}: {v}" for k, v in list(failures.items())[:5]))

    metrics = sorted({key for scores in results.values() for key in scores
                      if not key.startswith("_")})
    print(f"{'metric':<26} {'mean':>8} {'median':>8}")
    summary = {}
    for metric in metrics:
        values = [scores[metric] for scores in results.values() if metric in scores]
        summary[metric] = {"mean": statistics.fmean(values),
                           "median": statistics.median(values),
                           "files": len(values)}
        print(f"{metric:<26} {summary[metric]['mean']:>8.4f} "
              f"{summary[metric]['median']:>8.4f}")

    audio_seconds = sum(scores["_seconds"] for scores in results.values())
    print(f"\n{len(results)} files, {audio_seconds / 60:.1f} minutes of audio"
          + (f", {len(failures)} failed" if failures else ""))
    for name, message in list(failures.items())[:5]:
        print(f"  failed: {name}: {message}")
    print("\nHANDOFF §A.1 for reference: BeatNet+ 80.62 beat / 56.51 downbeat F-measure "
          "on GTZAN,\nat the same 70 ms tolerance. A different dataset is a different "
          "number; only GTZAN\ncompares directly.")

    if options.report:
        options.report.write_text(json.dumps({
            "cli": str(cli),
            "dataset": str(options.dataset),
            "annotations": str(options.annotations or options.dataset),
            "weights": options.weights,
            "octave_fold": options.bpm,
            "confidence_threshold": options.confidence,
            "seed": options.seed,
            "platform": platform.platform(),
            "files": len(results),
            "unmatched": unmatched,
            "failures": failures,
            "summary": summary,
            "per_file": results,
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        print(f"\nwrote {options.report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
