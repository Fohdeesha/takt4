"""Pseudo-label the operator's own library with Beat This!, laid out as a training set.

    python tools/train/distil.py "E:/Music/Library"                  # every audio file under it
    python tools/train/distil.py references/audio/playlists          # every track every .m3u there lists
    python tools/train/distil.py mix.m3u other.m3u8 "E:/More Music"  # any mix of the three
    python tools/train/distil.py ... --limit 20                      # a taste first
    python tools/train/distil.py ... --device cpu

The data the fine-tune is missing is the rig's own
music — its tempo band and its feel — and nobody has annotated it. Beat This! (CPJKU,
ISMIR 2024; the `final0` checkpoint at references/beat-this/final0.ckpt, the same system
the harness uses as its reference) reads each whole track non-causally and writes its
beats and downbeats; the small causal network is then trained to reproduce them. The
teacher's mistakes become the student's ceiling, so the 23 tracks of references/audio
are held out by layout.py (they are the test set), and the labels go through the same
checks as any other set's: `check.py`'s kick witness says whether the teacher's grid is
on the kick, `octave.py` puts drum-and-bass-style tracks at the operator's half time.

**Sources.** A directory is walked for audio files (flac, mp3, wav, m4a, ogg, aiff, opus,
wma); an .m3u/.m3u8 file is read for the paths it lists, relative ones taken from the
playlist's own directory; a directory that holds playlists is every playlist in it. The
playlists the operator exports are Windows-1252 text in which every character outside
that code page became `?` (2026-09-09: 50 of 1,328 lines read as UTF-8 did not exist,
16 still did not as Windows-1252), so each line is decoded both ways, and a path that
still does not exist is resolved against the disk component by component: `?` stands
for any one character, and failing that the child whose ASCII skeleton (letters and
digits only) is closest by difflib, at 0.8 or better and alone at the top, is taken.
What cannot be resolved is listed in `unresolved.txt`, and a track listed twice is
labelled once.

**Names.** A track is written as `<artist>-<album>-<file>`: its path below the naming root
(the common root of the first run's sources, kept in `naming_root.txt`), joined with `-`,
each part reduced to `[A-Za-z0-9._-]` — so the 23 harness tracks are recognisable to
layout.py's held-out check by their file names, and a `01 Intro` from one album does not
collide with another's. `sources.tsv` maps every stem back to the file it came from, and a
file keeps its stem on every later run, whatever else is added (`assign_stems`).

For every track this writes under references/datasets/library/ (git-ignored):

    audio/<stem>.wav          mono 22050 Hz 16-bit, decoded by ffmpeg (what every tool reads)
    annotations/<stem>.beats  '<seconds> TAB <beat in bar>', downbeat = 1, from Beat This!

Existing outputs are kept, so it can be stopped and restarted. Then:

    python tools/train/layout.py --sets library
    python tools/train/features.py --sets library
    python tools/train/check.py --sets library
    python tools/train/octave.py --sets library --all      # or the kick-parity rule
    python tools/train/finetune.py --config tools/train/configs/electronic-library.yaml
"""
import argparse
import difflib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import DATASETS, ROOT, SAMPLE_RATE, SETS, lower_priority, write_beats  # noqa: E402

SUFFIXES = {".flac", ".mp3", ".wav", ".m4a", ".ogg", ".aiff", ".aif", ".opus", ".wma"}
PLAYLISTS = {".m3u", ".m3u8"}
CHECKPOINT = ROOT / "references" / "beat-this" / "final0.ckpt"
MAX_STEM = 120


def safe_part(text):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("_")


def stem_for(src, root):
    """`<artist>-<album>-<file>`: the path below `root`, without the suffix; for a file
    outside `root`, its last three path components."""
    try:
        rel = src.relative_to(root)
    except ValueError:
        rel = Path(*src.parts[-3:])
    parts = [safe_part(p) for p in rel.parts[:-1]] + [safe_part(src.stem)]
    stem = "-".join(p for p in parts if p) or "track"
    if len(stem) > MAX_STEM:
        keep = (MAX_STEM - 1) // 2
        stem = stem[:keep] + "~" + stem[-keep:]
    return stem


def assign_stems(files, out):
    """({source: stem}, naming root): the stem a file had on an earlier run, and a new one
    only for a file never seen; `sources.tsv` records every stem ever given.

    The root used to be the common root of whatever this run was given, so a source from
    another folder or share moved it and renamed every track — and a track's stem is its id
    in the manifest, its split, its flags and its teacher_cross row (the 2026-09-25 audit's
    P14). The root is now fixed the first time and kept in `naming_root.txt`; a map written
    before then gives it, as the common root of the files it names."""
    tsv, root_file = out / "sources.tsv", out / "naming_root.txt"
    known, order = {}, []
    if tsv.exists():
        for line in tsv.read_text(encoding="utf-8").splitlines():
            stem, _, src = line.partition("\t")
            if stem and src:
                known.setdefault(os.path.normcase(src), (stem, src))
                order.append(os.path.normcase(src))
    if root_file.exists():
        root = Path(root_file.read_text(encoding="utf-8").strip())
    else:
        basis = [src for _, src in known.values()] or [str(p) for p in files]
        try:
            root = Path(os.path.commonpath(basis)) if len(basis) > 1 else Path(basis[0]).parent
        except ValueError:                                  # sources on different drives
            root = Path(Path(basis[0]).anchor)
        if root.is_file():
            root = root.parent
        out.mkdir(parents=True, exist_ok=True)
        root_file.write_text(f"{root}\n", encoding="utf-8", newline="\n")

    used = {stem: key for key, (stem, _) in known.items()}
    stems = {}
    for src in files:
        key = os.path.normcase(str(src))
        if key in known:
            stems[src] = known[key][0]
            continue
        stem = stem_for(src, root)
        if used.get(stem, key) != key:
            k = 2
            while used.get(f"{stem}_{k}", key) != key:
                k += 1
            stem = f"{stem}_{k}"
        used[stem] = key
        stems[src] = stem
        known[key] = (stem, str(src))
        order.append(key)
    out.mkdir(parents=True, exist_ok=True)
    with open(tsv, "w", encoding="utf-8", newline="\n") as f:
        for key in dict.fromkeys(order):
            f.write(f"{known[key][0]}\t{known[key][1]}\n")
    return stems, root


# ---------------------------------------------------------------------------------------
# Sources: directories, playlists, and the paths a playlist names
# ---------------------------------------------------------------------------------------

def skeleton(name):
    return "".join(ch for ch in name.lower() if ch.isascii() and ch.isalnum())


def track_number(name):
    m = re.match(r"\s*(\d{1,3})\b", name)
    return int(m.group(1)) if m else None


def _exists(path):
    try:
        return path.exists()
    except OSError:      # a `?` or `:` in a name is invalid on Windows and raises
        return False


def closest(part, children, want_file):
    """The child that most plausibly is `part` with its characters lost: the closest ASCII
    skeleton (a file's without its suffix), alone at the top, at 0.8 or better — or at
    0.5 when the two share a leading track number, which is how a re-ripped `02 Artist -
    Title.mp3` finds `02 - Title.flac`."""
    target = skeleton(Path(part).stem if want_file else part)
    number = track_number(part)
    scored = []
    for child in children:
        if child.is_file() != want_file:
            continue
        name = child.stem if want_file else child.name
        ratio = difflib.SequenceMatcher(None, target, skeleton(name)).ratio()
        threshold = 0.5 if (number is not None and track_number(name) == number) else 0.8
        scored.append((ratio, ratio >= threshold, child))
    scored.sort(key=lambda s: s[0], reverse=True)
    if scored and scored[0][1] and (len(scored) == 1 or scored[0][0] > scored[1][0]):
        return scored[0][2]
    return None


def resolve_parts(cur, parts):
    """The file `parts` names below `cur`, one component at a time: exact, then `?` as a
    one-character wildcard, then the closest name in the folder (`closest`), and for a
    folder that is not there under any name, the same file under any of its siblings —
    the album was renamed, or the track moved to another album of the artist's."""
    if not parts:
        return cur if cur.is_file() else None
    part, rest = parts[0], parts[1:]
    if _exists(cur / part):
        found = resolve_parts(cur / part, rest)
        if found is not None:
            return found
    try:
        children = list(cur.iterdir())
    except OSError:
        return None
    if "?" in part:
        pattern = re.compile("^" + "".join("." if ch == "?" else re.escape(ch) for ch in part) + "$", re.I)
        hits = [c for c in children if pattern.match(c.name) and c.is_file() == (not rest)]
        if len(hits) == 1:
            found = resolve_parts(hits[0], rest)
            if found is not None:
                return found
    match = closest(part, children, want_file=not rest)
    if match is not None:
        found = resolve_parts(match, rest)
        if found is not None:
            return found
    if rest:
        siblings = [c for c in children if c.is_dir()]
        if len(siblings) <= 50:      # an artist's albums, not the whole library
            found = [f for f in (resolve_parts(c, rest) for c in siblings) if f is not None]
            if len(found) == 1:
                return found[0]
    return None


def resolve(candidate: Path):
    """The file `candidate` names, or the one it most plausibly names when the playlist's
    text has lost characters (see the module docstring)."""
    if _exists(candidate) and candidate.is_file():
        return candidate
    parts = candidate.parts
    if not parts or not _exists(Path(parts[0])):
        return None
    return resolve_parts(Path(parts[0]), parts[1:])


def playlist_entries(path: Path):
    """The audio files an .m3u names, resolved; and the lines that could not be."""
    raw = path.read_bytes().replace(b"\r\n", b"\n")
    found, unresolved = [], []
    for line in raw.split(b"\n"):
        line = line.strip()
        if not line or line.startswith(b"#"):
            continue
        texts = []
        try:
            texts.append(line.decode("utf-8"))
        except UnicodeDecodeError:
            pass
        latin = line.decode("cp1252", errors="replace")
        if latin not in texts:
            texts.append(latin)
        hit = None
        for text in texts:
            candidate = Path(text)
            if not candidate.is_absolute():
                candidate = path.parent / candidate
            hit = resolve(candidate)
            if hit is not None:
                break
        if hit is not None and hit.suffix.lower() in SUFFIXES:
            found.append(hit.resolve())
        else:
            unresolved.append((str(path.name), texts[-1]))
    return found, unresolved


def gather(sources):
    """(sorted unique audio files, unresolved playlist lines) from any mix of directories
    of audio, playlists, and directories of playlists."""
    files, unresolved = set(), []
    for source in sources:
        source = Path(source)
        if source.is_file() and source.suffix.lower() in PLAYLISTS:
            playlists = [source]
        elif source.is_dir():
            playlists = sorted(p for p in source.iterdir() if p.is_file() and p.suffix.lower() in PLAYLISTS)
            if not playlists:
                files.update(p.resolve() for p in source.rglob("*")
                             if p.is_file() and p.suffix.lower() in SUFFIXES)
                continue
        else:
            raise SystemExit(f"{source} is neither a directory nor a playlist")
        for playlist in playlists:
            found, missing = playlist_entries(playlist)
            files.update(found)
            unresolved.extend(missing)
            print(f"  {playlist.name}: {len(found)} tracks resolved, {len(missing)} not", flush=True)
    return sorted(files), unresolved


# ---------------------------------------------------------------------------------------
# Decode and label
# ---------------------------------------------------------------------------------------

def decode(src, dst):
    r = subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(src), "-vn", "-ac", "1",
                        "-ar", str(SAMPLE_RATE), "-sample_fmt", "s16", str(dst)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip()[:200])


class Teacher:
    """Beat This!, one checkpoint or an ensemble of them (2026-09-09: `final0`, `final1`,
    `final2` — the three seeds the paper trained on all its data; the ensemble of the
    three is less wrong than any one of them where the material is ambiguous, and where
    the seeds disagree with *each other* the label is a guess, which `agreement` reports).

    The spectrogram is computed once; every model's frame logits are taken; the mean of
    their probabilities, as logits again, goes through the package's own peak picking
    (`Postprocessor("minimal")`, peaks above 0.5 within ±70 ms), exactly as one model's
    would. Each seed's own beats are kept for the agreement."""

    def __init__(self, checkpoints, device):
        from beat_this.inference import Audio2Frames
        from beat_this.model.postprocessor import Postprocessor
        self.models = [Audio2Frames(checkpoint_path=str(c), device=device) for c in checkpoints]
        self.post = Postprocessor(type="minimal")

    def __call__(self, signal, sr):
        import torch
        spect = self.models[0].signal2spect(signal, sr)
        logits = [m.spect2frames(spect) for m in self.models]
        per_seed = [self.post(b, d) for b, d in logits]
        if len(logits) == 1:
            return per_seed[0], per_seed
        pb = torch.stack([b.sigmoid() for b, _ in logits]).mean(0).clamp(1e-6, 1 - 1e-6)
        pd = torch.stack([d.sigmoid() for _, d in logits]).mean(0).clamp(1e-6, 1 - 1e-6)
        return self.post(torch.logit(pb), torch.logit(pd)), per_seed


def agreement(per_seed):
    """(min, mean) beat F-measure between the seeds' own beats, pairwise; (1, 1) for one."""
    import mir_eval
    if len(per_seed) < 2:
        return 1.0, 1.0
    scores = []
    for i in range(len(per_seed)):
        for j in range(i + 1, len(per_seed)):
            a = mir_eval.beat.trim_beats(np.asarray(per_seed[i][0], float))
            b = mir_eval.beat.trim_beats(np.asarray(per_seed[j][0], float))
            scores.append(mir_eval.beat.f_measure(a, b) if len(a) and len(b) else 0.0)
    return float(min(scores)), float(np.mean(scores))


def positions(beats, downbeats):
    """Beat-in-bar labels from beats and downbeats (tools/refeval/ref_beatthis.py's rule)."""
    labels = np.zeros(len(beats), dtype=int)
    down = set(np.round(downbeats, 4))
    count, seen = 0, False
    for i, b in enumerate(beats):
        if round(float(b), 4) in down:
            count, seen = 1, True
        elif seen:
            count += 1
        labels[i] = count if seen else 0
    return labels


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sources", nargs="+", type=Path,
                    help="directories of music, .m3u/.m3u8 playlists, or directories of playlists")
    ap.add_argument("--out", type=Path, default=DATASETS / SETS["library"][0])
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--checkpoints", nargs="+", default=["final0", "final1", "final2"],
                    help="Beat This! checkpoints under references/beat-this/ (names) or paths; "
                         "several are averaged, and their agreement written to agreement.tsv")
    a = ap.parse_args(argv)
    lower_priority()
    checkpoints = [Path(c) if Path(c).is_file() else CHECKPOINT.with_name(f"{c}.ckpt") for c in a.checkpoints]
    for c in checkpoints:
        if not c.is_file():
            raise SystemExit(f"{c} missing; see tools/refeval/ref_beatthis.py for where it comes from")

    files, unresolved = gather(a.sources)
    a.out.mkdir(parents=True, exist_ok=True)
    if unresolved:
        with open(a.out / "unresolved.txt", "w", encoding="utf-8", newline="\n") as f:
            for playlist, text in unresolved:
                f.write(f"{playlist}\t{text}\n")
        print(f"{len(unresolved)} playlist lines could not be resolved to a file; "
              f"listed in {a.out / 'unresolved.txt'}", flush=True)
    if not files:
        raise SystemExit("no audio files found")
    stems, root = assign_stems(files, a.out)

    if a.limit:
        files = files[:a.limit]
    print(f"{len(files)} audio files below {root} -> {a.out} on {a.device}", flush=True)

    import soundfile as sf
    import torch
    device = a.device if (a.device == "cpu" or torch.cuda.is_available()) else "cpu"
    teacher = Teacher(checkpoints, device)
    torch.set_num_threads(4)
    print(f"teacher: {', '.join(c.stem for c in checkpoints)} on {device}", flush=True)

    audio_dir, ann_dir = a.out / SETS["library"][1], a.out / SETS["library"][2]
    audio_dir.mkdir(parents=True, exist_ok=True)
    ann_dir.mkdir(parents=True, exist_ok=True)
    # The seeds' agreement per track, appended to as it goes; layout.py reads it.
    agreement_path = a.out / "agreement.tsv"
    seen = set()
    if agreement_path.exists():
        with open(agreement_path, encoding="utf-8") as f:
            seen = {line.split("\t")[0] for line in f if line.strip()}

    done, skipped, failed, t0 = 0, 0, [], time.time()
    for src in files:
        stem = stems[src]
        wav, beats = audio_dir / f"{stem}.wav", ann_dir / f"{stem}.beats"
        if wav.exists() and beats.exists():
            skipped += 1
            continue
        try:
            if not wav.exists():
                decode(src, wav)
            y, sr = sf.read(str(wav), dtype="float32")
            (b, d), per_seed = teacher(y, sr)
            if len(b) < 8:
                raise RuntimeError(f"Beat This! found {len(b)} beats")
            write_beats(beats, b, positions(b, d))
            low, mean = agreement(per_seed)
            tempo = 60.0 / np.median(np.diff(b))
            if stem not in seen:
                with open(agreement_path, "a", encoding="utf-8", newline="\n") as f:
                    tempi = "\t".join(f"{60.0 / np.median(np.diff(sb)):.1f}" if len(sb) > 2 else "0"
                                      for sb, _ in per_seed)
                    f.write(f"{stem}\t{low:.4f}\t{mean:.4f}\t{tempo:.1f}\t{tempi}\n")
            done += 1
            if done % 25 == 0 or done < 5:
                print(f"  {done:4d} {stem[:60]:<60} {len(b):5d} beats ~{tempo:6.1f} BPM  agree {low:.2f}  "
                      f"({(time.time() - t0) / done:.1f} s/track)", flush=True)
        except Exception as e:  # noqa: BLE001 - keep going, list at the end
            failed.append((str(src), repr(e)[:160]))
            print(f"  FAILED {src.name}: {repr(e)[:120]}", flush=True)
    print(f"done: {done} labelled, {skipped} already there, {len(failed)} failed, "
          f"{(time.time() - t0) / 60:.1f} min")
    if failed:
        with open(a.out / "failed.txt", "w", encoding="utf-8", newline="\n") as f:
            for src, why in failed:
                f.write(f"{src}\t{why}\n")
        print(f"failures listed in {a.out / 'failed.txt'}")


if __name__ == "__main__":
    main(sys.argv[1:])
