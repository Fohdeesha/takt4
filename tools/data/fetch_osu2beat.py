"""Fetch the audio behind osu2beat2025's annotations, from the public osu! beatmap mirrors,
as the mono 22050 Hz WAV every tool in this project reads.

    python tools/data/fetch_osu2beat.py            # all 708 audios
    python tools/data/fetch_osu2beat.py --limit 3  # a taste first

The dataset (`references/datasets/osu2mir/osu2beat2025_metered_beats.zip`, from
github.com/ziyunliu4444/osu2mir) is 741 annotation files named
`<md5 of the audio>_<beatmap set id>_beats_metered.txt`; the audio is inside the `.osz`
beatmap package of that set, which the authors say to fetch from osu.ppy.sh with an
account. The public mirrors serve the same packages without one — catboy.best first,
then nerinyan.moe, osu.direct and beatconnect.io — and the MD5 in the filename is what
says the audio is the one that was annotated: a package whose audio has since been
replaced fails the hash and is listed in `audio/failed.txt` rather than kept.

What it writes, under `references/datasets/osu2mir/` (git-ignored): `annotations/` — the
zip unpacked; `osz/<set>.osz` — the packages, kept; `audio/<md5>.wav` — one per distinct
audio, mono 22050 Hz 16-bit, named by the MD5 so an annotation finds its audio by its own
first field. Restartable.
"""
import argparse
import hashlib
import io
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SET = ROOT / "references" / "datasets" / "osu2mir"
ARCHIVE = SET / "osu2beat2025_metered_beats.zip"
ANNOTATIONS = SET / "annotations"
OSZ = SET / "osz"
AUDIO = SET / "audio"
MIRRORS = ("https://catboy.best/d/{id}", "https://api.nerinyan.moe/d/{id}",
           "https://osu.direct/api/d/{id}", "https://beatconnect.io/b/{id}")
AUDIO_SUFFIXES = (".mp3", ".ogg", ".wav", ".flac", ".m4a")


def wanted():
    """{beatmap set id: {md5, ...}} from the annotation filenames."""
    ANNOTATIONS.mkdir(parents=True, exist_ok=True)
    if not any(ANNOTATIONS.iterdir()):
        with zipfile.ZipFile(ARCHIVE) as z:
            z.extractall(ANNOTATIONS)
    sets = {}
    for p in ANNOTATIONS.glob("*_beats_metered.txt"):
        md5, set_id = p.name.split("_")[:2]
        sets.setdefault(set_id, set()).add(md5.lower())
    return sets


def download(set_id):
    target = OSZ / f"{set_id}.osz"
    if target.exists() and target.stat().st_size > 0:
        return target, ""
    last = ""
    for mirror in MIRRORS:
        try:
            req = urllib.request.Request(mirror.format(id=set_id),
                                         headers={"User-Agent": "takt4-dataset-fetch/1.0"})
            with urllib.request.urlopen(req, timeout=180) as r:
                data = r.read()
            if len(data) < 1000 or not zipfile.is_zipfile(io.BytesIO(data)):
                last = f"{mirror}: not a beatmap package ({len(data)} bytes)"
                continue
            target.write_bytes(data)
            return target, ""
        except Exception as e:  # noqa: BLE001 - try the next mirror
            last = f"{mirror}: {e}"
    return None, last


def fetch(item):
    set_id, md5s = item
    missing = [m for m in md5s if not (AUDIO / f"{m}.wav").exists()]
    if not missing:
        return set_id, "exists", ""
    package, reason = download(set_id)
    if package is None:
        return set_id, "failed", reason
    found = 0
    with zipfile.ZipFile(package) as z:
        for entry in z.infolist():
            if not entry.filename.lower().endswith(AUDIO_SUFFIXES):
                continue
            data = z.read(entry)
            md5 = hashlib.md5(data).hexdigest()
            if md5 not in missing:
                continue
            with tempfile.TemporaryDirectory() as scratch:
                src = Path(scratch) / ("audio" + Path(entry.filename).suffix.lower())
                src.write_bytes(data)
                r = subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(src),
                                    "-ac", "1", "-ar", "22050", "-sample_fmt", "s16",
                                    str(AUDIO / f"{md5}.wav")], capture_output=True, text=True)
            if r.returncode == 0:
                found += 1
            else:
                return set_id, "failed", f"ffmpeg: {r.stderr.strip()[:160]}"
    if found < len(missing):
        return set_id, "failed", (f"{len(missing) - found} of {len(missing)} annotated audios not "
                                  f"in the package (audio replaced since the paper?)")
    return set_id, "ok", ""


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--jobs", type=int, default=3, help="polite to the mirrors: 3")
    a = ap.parse_args(argv)
    OSZ.mkdir(parents=True, exist_ok=True)
    AUDIO.mkdir(parents=True, exist_ok=True)
    sets = sorted(wanted().items())
    if a.limit:
        sets = sets[:a.limit]
    total_audio = sum(len(m) for _, m in sets)
    print(f"osu2beat2025: {len(sets)} beatmap sets, {total_audio} distinct audios -> {AUDIO}",
          flush=True)
    counts = {"ok": 0, "exists": 0, "failed": 0}
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for set_id, status, reason in pool.map(fetch, sets):
            counts[status] += 1
            if status == "failed":
                with open(AUDIO / "failed.txt", "a", encoding="utf-8") as f:
                    f.write(f"{set_id}\t{reason}\n")
            if status != "exists":
                print(f"  {status:<7} {set_id}  {reason}", flush=True)
    print(f"done: {counts['ok']} sets fetched, {counts['exists']} already there, "
          f"{counts['failed']} failed; {len(list(AUDIO.glob('*.wav')))} audios on disk")


if __name__ == "__main__":
    main(sys.argv[1:])
