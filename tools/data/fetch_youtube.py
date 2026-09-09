"""Fetch the audio a beat-tracking dataset only names, from YouTube, as the mono 22050 Hz
WAV every tool in this project reads.

    python tools/data/fetch_youtube.py harmonix    # references/datasets/harmonixset -> audio/
    python tools/data/fetch_youtube.py raveform    # references/datasets/raveform -> audio/
    python tools/data/fetch_youtube.py raveform --limit 5   # a taste first

The datasets ship annotations and YouTube identifiers, not audio — Harmonix
(`dataset/youtube_urls.csv`) and Raveform (`tracks.jsonl`) alike — so this is the step
their own READMEs leave to the user. Everything lands under `references/`, which is
git-ignored; nothing fetched here can reach the repository.

What it writes, per track: `<out>/<track id>.wav`, mono, 22050 Hz, 16-bit — decoded by
ffmpeg on the way in, so nothing downstream has to read a webm. Tracks that fail (taken
down, region-locked, private) are listed in `<out>/failed.txt` with yt-dlp's reason and
skipped next time; tracks already present are skipped too, so this can be stopped and
restarted at will. It runs yt-dlp four at a time; expect a few seconds a track and a
rate limit now and then, which yt-dlp waits out.

yt-dlp is in the project venv (`pip install yt-dlp`) and ffmpeg has to be on PATH.
"""
import argparse
import csv
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATASETS = ROOT / "references" / "datasets"
PYTHON = Path(sys.executable)


def harmonix_tracks():
    """(track id, YouTube URL) from the Harmonix metadata."""
    with open(DATASETS / "harmonixset" / "dataset" / "youtube_urls.csv", newline="",
              encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row.get("URL"):
                yield row["File"], row["URL"]


def raveform_tracks(annotated_only=True):
    """(track id, YouTube URL) from Raveform's `tracks.jsonl`, whose `id` is the YouTube id.

    Only the annotated tracks by default — the 1,423 with a `structures/beats/
    <index>.<id>.beat.csv` (time, beat in bar, section) — which is what the fine-tune
    wants, not the 61,886 the mixes name. Read from the unpacked `raveform/` directory,
    or straight out of `raveform.zip` if it has not been unpacked yet.
    """
    import zipfile

    base = DATASETS / "raveform"
    archive = DATASETS / "raveform.zip"
    if (base / "tracks.jsonl").exists() and (base / "structures" / "beats").is_dir():
        annotated = {p.name.split(".")[1] for p in (base / "structures" / "beats").glob("*.beat.csv")}
        with open(base / "tracks.jsonl", encoding="utf-8") as f:
            rows = [json.loads(line) for line in f]
    elif archive.exists():
        with zipfile.ZipFile(archive) as z:
            annotated = {n.split("/")[-1].split(".")[1] for n in z.namelist()
                         if n.startswith("raveform/structures/beats/") and n.endswith(".beat.csv")}
            with z.open("raveform/tracks.jsonl") as f:
                rows = [json.loads(line) for line in f]
    else:
        raise SystemExit(f"neither {base} nor {archive}: fetch raveform.zip from HuggingFace first")
    for row in rows:
        track_id = str(row.get("id") or "")
        if not track_id:
            continue
        if annotated_only and track_id not in annotated:
            continue
        yield track_id, f"https://www.youtube.com/watch?v={track_id}"


SOURCES = {
    "harmonix": (harmonix_tracks, DATASETS / "harmonixset" / "audio"),
    "raveform": (raveform_tracks, DATASETS / "raveform" / "audio"),
}


def fetch(track_id, url, out_dir):
    target = out_dir / f"{track_id}.wav"
    if target.exists():
        return track_id, "exists", ""
    cmd = [str(PYTHON), "-m", "yt_dlp", "--no-playlist", "--quiet", "--no-warnings",
           "-f", "bestaudio/best", "-x", "--audio-format", "wav",
           "--postprocessor-args", "ffmpeg:-ac 1 -ar 22050 -sample_fmt s16",
           "-o", str(out_dir / f"{track_id}.%(ext)s"), url]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not target.exists():
        reason = (r.stderr.strip() or r.stdout.strip()).splitlines()[-1:] or ["no output"]
        return track_id, "failed", reason[0][:200]
    return track_id, "ok", ""


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source", choices=list(SOURCES))
    ap.add_argument("--limit", type=int, default=None, help="stop after this many tracks")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--retry-failed", action="store_true",
                    help="try again the tracks failed.txt lists")
    a = ap.parse_args(argv)

    tracks_of, out_dir = SOURCES[a.source]
    out_dir.mkdir(parents=True, exist_ok=True)
    failed_log = out_dir / "failed.txt"
    failed = {}
    if failed_log.exists():
        for line in failed_log.read_text(encoding="utf-8").splitlines():
            if "\t" in line:
                tid, reason = line.split("\t", 1)
                failed[tid] = reason
    tracks = [(tid, url) for tid, url in tracks_of()
              if a.retry_failed or tid not in failed]
    if a.limit:
        tracks = tracks[:a.limit]
    print(f"{a.source}: {len(tracks)} tracks to consider -> {out_dir}", flush=True)

    counts = {"ok": 0, "exists": 0, "failed": 0}
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for tid, status, reason in pool.map(lambda t: fetch(t[0], t[1], out_dir), tracks):
            counts[status] += 1
            if status == "failed":
                failed[tid] = reason
                with open(failed_log, "a", encoding="utf-8") as f:
                    f.write(f"{tid}\t{reason}\n")
            elif tid in failed and a.retry_failed:
                failed.pop(tid, None)
            if status != "exists":
                print(f"  {status:<7} {tid}  {reason}", flush=True)
    if a.retry_failed:
        failed_log.write_text("".join(f"{k}\t{v}\n" for k, v in failed.items()), encoding="utf-8")
    print(f"done: {counts['ok']} fetched, {counts['exists']} already there, {counts['failed']} failed"
          + (f" (see {failed_log})" if counts["failed"] else ""))


if __name__ == "__main__":
    main(sys.argv[1:])
