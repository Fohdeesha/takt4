"""Fetch the GiantSteps tempo dataset's 664 Beatport previews, MD5-verified, as the mono
22050 Hz WAV every tool in this project reads.

    python tools/data/fetch_giantsteps.py

What the dataset's own `audio_dl.sh` does, in Python and on Windows: one MP3 per `md5/`
file from the JKU host (the Beatport host it falls back to returns 404 now), checked
against the committed MD5, then decoded by ffmpeg to `<repo>/references/datasets/
giantsteps-tempo/audio/<id>.wav`. The MP3s are kept beside them. Restartable; failures go
to `audio/failed.txt`. Everything lands under `references/`, which is git-ignored.
"""
import hashlib
import ssl
import subprocess
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SET = ROOT / "references" / "datasets" / "giantsteps-tempo"
HOST = "https://www.cp.jku.at/datasets/giantsteps/backup/"
OUT = SET / "audio"

#: The JKU host's certificate had expired on 2026-09-08 and every download failed on it.
#: Verification is switched off for this host only, because the file's integrity is
#: established by the dataset's own committed MD5, which is checked below on every file —
#: a substituted download fails the hash and is deleted.
INSECURE = ssl._create_unverified_context()  # noqa: SLF001


def fetch(md5_file: Path):
    stem = md5_file.stem  # e.g. 2022116.LOFI
    mp3 = OUT / f"{stem}.mp3"
    wav = OUT / f"{stem}.wav"
    if wav.exists():
        return stem, "exists", ""
    want = md5_file.read_text(encoding="utf-8").strip().lower()
    try:
        if not mp3.exists():
            with urllib.request.urlopen(HOST + f"{stem}.mp3", timeout=120, context=INSECURE) as r:
                mp3.write_bytes(r.read())
        got = hashlib.md5(mp3.read_bytes()).hexdigest()
        if got != want:
            mp3.unlink(missing_ok=True)
            return stem, "failed", f"md5 {got} != {want}"
        r = subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(mp3), "-ac", "1",
                            "-ar", "22050", "-sample_fmt", "s16", str(wav)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not wav.exists():
            return stem, "failed", (r.stderr.strip() or "ffmpeg failed")[:200]
        return stem, "ok", ""
    except Exception as e:  # noqa: BLE001 - one bad file must not stop the run
        return stem, "failed", str(e)[:200]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    files = sorted((SET / "md5").glob("*.md5"))
    print(f"giantsteps: {len(files)} tracks -> {OUT}", flush=True)
    counts = {"ok": 0, "exists": 0, "failed": 0}
    failed = OUT / "failed.txt"
    with ThreadPoolExecutor(max_workers=4) as pool:
        for stem, status, reason in pool.map(fetch, files):
            counts[status] += 1
            if status == "failed":
                with open(failed, "a", encoding="utf-8") as f:
                    f.write(f"{stem}\t{reason}\n")
                print(f"  failed  {stem}  {reason}", flush=True)
    print(f"done: {counts['ok']} fetched, {counts['exists']} already there, {counts['failed']} failed")


if __name__ == "__main__":
    sys.exit(main())
