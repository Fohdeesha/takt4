"""Where the reference-evaluation harness keeps its files, and the readers every script
in this directory shares.

Two kinds of data, kept apart on purpose:

  * **Committed**: the two offline reference systems' beat files, under
    `tests/data/tracking/refeval/{ref_beatthis,ref_madmom}/`, one `<stem>.beats` per
    track in the Ballroom layout (`<seconds> TAB <beat in bar>`, downbeat = 1). Forty-six
    text files, a quarter of a megabyte. Every score in TRACKING-PROPOSAL.md is measured
    against them, and `gate.py` needs them to be the same files on every machine — so
    they are annotations in the repository, like `tests/data/tracking/evaluation/`, and
    not audio.
  * **Regenerated**: the decoded audio (~480 MB), takt4's traces and beat files, and the
    alternative decoders' output, under the git-ignored `references/refeval-work/`.
    `decode_all.py` and `run_takt4.py` rebuild the first two in a few minutes.

Override either with TAKT4_REFEVAL_WORK / TAKT4_REFBEATS, and the CLI with TAKT4_CLI.
"""
import os
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get("TAKT4_REFEVAL_WORK", ROOT / "references" / "refeval-work"))
REFS = Path(os.environ.get("TAKT4_REFBEATS", ROOT / "tests" / "data" / "tracking" / "refeval"))
AUDIO = WORK / "audio"      # decode_all.py: <stem>.wav, mono 22050 Hz float32
TAKT4 = WORK / "takt4"      # run_takt4.py: <stem>.<tag>.trace and .beats
DEC = WORK / "dec"          # decoders.py: <stem>.<variant>.beats
WORK.mkdir(parents=True, exist_ok=True)

#: The network runs at 50 frames a second; the traces are written one row per frame.
FPS = 50.0

#: The tracks where the two references disagree (beat F < 0.8 against each other), by
#: stem. A score on these measures agreement with a convention rather than correctness —
#: TRACKING-PROPOSAL.md §1 — so `score.py --min-agree 0.8` reports the other fifteen
#: separately and `gate.py` gates on both sets.
HARD_EIGHT = (
    "03_Trigger_Finger",
    "04_Jensen_Interceptor_Model_2029",
    "04_The_Galaxist",
    "05_Alias_Getting_By_version_2",
    "08_True_Believer",
    "09_Defang",
    "09_Jack_Yourself",
    "19_daOooooh",
)


def cli_path():
    """The takt4-cli to measure: TAKT4_CLI, or the local Release build."""
    given = os.environ.get("TAKT4_CLI")
    candidates = [Path(given)] if given else [
        ROOT / "build" / "windows-msvc" / "bin" / "Release" / "takt4-cli.exe",
        ROOT / "build" / "windows-core" / "bin" / "Release" / "takt4-cli.exe",
        ROOT / "build" / "linux-core" / "bin" / "takt4-cli",
    ]
    for path in candidates:
        if path.is_file():
            return path
    raise SystemExit("no takt4-cli found; set TAKT4_CLI. Looked in:\n  " +
                     "\n  ".join(str(p) for p in candidates))


def system_dir(spec):
    """A system argument `DIR` or `DIR:TAG` -> (spec, directory, tag).

    `DIR` is looked up under the work tree first (takt4, dec) and then among the committed
    references (ref_beatthis, ref_madmom), so a reference can be scored as a system.
    """
    d, _, tag = spec.partition(":")
    directory = WORK / d if (WORK / d).exists() else REFS / d
    return spec, directory, tag


def beats_path(directory, stem, tag):
    return directory / (f"{stem}.{tag}.beats" if tag else f"{stem}.beats")


def load_beats(path):
    """(times, beat-in-bar labels, published BPM per beat or None) from a .beats file."""
    rows = np.loadtxt(path, ndmin=2)
    if rows.size == 0:
        return np.zeros(0), np.zeros(0, dtype=int), None
    times = rows[:, 0]
    labels = rows[:, 1].astype(int) if rows.shape[1] > 1 else np.zeros(len(times), dtype=int)
    bpm = rows[:, 2] if rows.shape[1] > 2 else None
    return times, labels, bpm


def write_beats(path, times, labels):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for t, l in zip(times, labels):
            f.write(f"{t:.4f}\t{int(l)}\n")


def stems(ref="ref_beatthis"):
    """Every track the reference has beats for, by stem, sorted."""
    return sorted(p.stem for p in (REFS / ref).glob("*.beats"))


def load_trace(stem, tag="nofold", network_only=True):
    """One takt4 trace as a structured array, by column name.

    A decoder faster than the network is fed frames the engine interpolates between the
    network's (`interp` = 1 in the trace). By default those are dropped, so a trace is the
    network's 50 Hz frames whichever decoder wrote it and `FPS` applies; pass
    `network_only=False` for every decoder frame, at the rate `1 / (time[1] - time[0])`.
    """
    rows = np.genfromtxt(TAKT4 / f"{stem}.{tag}.trace", delimiter="\t", names=True)
    if network_only and "interp" in (rows.dtype.names or ()):
        rows = rows[rows["interp"] == 0]
    return rows
