"""Annotate a track by tapping along to it, from any audio format.

    python tools/annotate.py "D:/Music/Artist/Album/03 Track.flac"      # <track>.beats beside it
    python tools/annotate.py track.flac --dataset                        # ...and into references/datasets/operator/
    python tools/annotate.py --harness Defang                            # one of the 23, into tests/data/tracking/refeval/ref_operator/
    python tools/annotate.py track.flac --taps track.taps                # no playback: redo from the taps kept last time
    python tools/annotate.py track.flac -- --snap off --octave half      # anything after -- goes to takt4-cli annotate
    python tools/annotate.py --outputs                                   # the output devices, for --output

The console plays the track — through the system's output, or `-- --output NAME` — and takes
the taps: space on a beat, d on a downbeat, u undoes one, h and x set the octave the file is
written at (a half-time tap on drum and bass records the operator's octave as the label), s
switches the snap to the network's peaks, q finishes. What the CLI writes — the .beats file
and the raw .taps beside it — goes where asked:

  * beside the source track (the default), as <stem>.beats and <stem>.taps;
  * --dataset: also into references/datasets/operator/{audio/<id>.wav, annotations/<id>.beats},
    the layout tools/train/layout.py reads (`--sets operator`), <id> being the source's
    artist-album-file stem as tools/train/distil.py names the library's;
  * --harness NAME: for one of references/audio's 23 tracks (NAME a substring of its stem in
    tests/data/tracking/refeval/ref_beatthis/), into ref_operator/<stem>.beats beside the two
    machine references, so `tools/refeval/score.py --ref ref_operator ...` scores the trackers
    against the operator's own beats — the ground truth the project otherwise lacks. The
    WAV is the harness's own decode (tools/refeval/decode_all.py).

The decode is ffmpeg's, to the mono 22050 Hz WAV every tool reads, kept under
references/refeval-work/annotate/ so a second pass over the same track does not decode again.
This has to run in a real console (cmd, PowerShell, Windows Terminal): the keys come from it.
"""
import argparse
import importlib.util
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


refeval = load("refeval_common", ROOT / "tools" / "refeval" / "common.py")
train = load("train_common", ROOT / "tools" / "train" / "common.py")
distil = load("train_distil", ROOT / "tools" / "train" / "distil.py")

SAMPLE_RATE = 22050
DECODED = refeval.WORK / "annotate"
OPERATOR_SET = train.SETS["operator"]
OPERATOR = train.DATASETS / OPERATOR_SET[0]


def slug(name):
    return re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_")


def decode(src, dst):
    if dst.exists():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(src), "-vn", "-ac", "1",
                        "-ar", str(SAMPLE_RATE), "-sample_fmt", "s16", str(dst)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"ffmpeg could not decode {src}: {r.stderr.strip()[:300]}")


def harness_stem(name):
    stems = refeval.stems("ref_beatthis")
    hits = [s for s in stems if name.lower() in s.lower()]
    if len(hits) != 1:
        raise SystemExit(f"--harness {name!r} matches {len(hits)} of the 23: {hits or stems}")
    return hits[0]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 epilog="options after -- are passed to `takt4-cli annotate`")
    ap.add_argument("track", nargs="?", type=Path, help="the audio file, in any format ffmpeg reads")
    ap.add_argument("--harness", metavar="NAME", help="annotate one of references/audio's 23 as the operator's reference")
    ap.add_argument("--dataset", action="store_true", help="also file the result under references/datasets/operator/")
    ap.add_argument("--taps", type=Path, help="no playback: the taps from this file (the .taps kept last time)")
    ap.add_argument("--out", type=Path, help="where the .beats goes; default beside the track")
    ap.add_argument("--outputs", action="store_true", help="list the output devices and exit")
    ap.add_argument("--cli", type=Path, help="takt4-cli to run; default the local Release build")
    if "--" in argv:
        split = argv.index("--")
        argv, extra = argv[:split], argv[split + 1:]
    else:
        extra = []
    a = ap.parse_args(argv)
    cli = a.cli or refeval.cli_path()

    if a.outputs:
        return subprocess.run([str(cli), "annotate", "--outputs"]).returncode

    if a.harness:
        stem = harness_stem(a.harness)
        wav = refeval.AUDIO / f"{stem}.wav"
        if not wav.exists():
            raise SystemExit(f"{wav} is missing; run tools/refeval/decode_all.py first")
        out_dir = refeval.REFS / "ref_operator"
        out_dir.mkdir(parents=True, exist_ok=True)
        beats = a.out or out_dir / f"{stem}.beats"
        taps = out_dir / f"{stem}.taps"
        source = None
    elif a.track:
        source = a.track.resolve()
        if not source.is_file():
            raise SystemExit(f"{source} is not a file")
        wav = DECODED / f"{slug(source.stem)}.wav"
        decode(source, wav)
        beats = a.out or source.with_suffix(".beats")
        taps = beats.with_suffix(".taps")
    else:
        ap.error("give a track, or --harness NAME")
        return 2

    cmd = [str(cli), "annotate", str(wav), "--out", str(beats), "--save-taps", str(taps)]
    if a.taps:
        cmd += ["--taps", str(a.taps)]
    cmd += extra
    print(" ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    r = subprocess.run(cmd)
    if r.returncode != 0 or not Path(beats).exists():
        return r.returncode or 1

    if a.dataset and source is not None:
        root = source.parents[2] if len(source.parents) > 2 else source.parent
        stem = distil.stem_for(source, root)
        audio_dir, ann_dir = OPERATOR / OPERATOR_SET[1], OPERATOR / OPERATOR_SET[2]
        audio_dir.mkdir(parents=True, exist_ok=True)
        ann_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(wav, audio_dir / f"{stem}.wav")
        shutil.copyfile(beats, ann_dir / f"{stem}.beats")
        shutil.copyfile(taps, ann_dir / f"{stem}.taps")
        with open(OPERATOR / "sources.tsv", "a", encoding="utf-8", newline="\n") as f:
            f.write(f"{stem}\t{source}\n")
        print(f"filed as the operator set's {stem}; next: python tools/train/layout.py --sets operator")
    if a.harness:
        print(f"the harness scores against it with: python tools/refeval/score.py --ref ref_operator takt4:nofold")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
